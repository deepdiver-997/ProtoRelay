#include "mail_system/back/bootstrap.h"

#include "framework/thread_pool/io_thread_pool.h"
#include "framework/thread_pool/worker_thread_pool.h"
#include "framework/log/log_macros.h"
#include "framework/storage/io_error.h"
#include "mail_system/back/router/hash_shard_router.h"
#include "mail_system/back/router/static_shard_router.h"
#include "mail_system/back/router/table_shard_router.h"
#include "mail_system/back/db/distributed_mysql_pool.h"
#include "mail_system/back/db/mysql_service.h"
#include "mail_system/back/db/null_db_pool.h"
#include "mail_system/back/storage/async_storage_provider.h"
#include "mail_system/back/storage/distributed_file_storage_provider.h"
#if PROTORELAY_ENABLE_HDFS_WEB_STORAGE
#include "mail_system/back/storage/hdfs_web_storage_provider.h"
#endif
#include "mail_system/back/storage/local_file_storage_provider.h"
#include "mail_system/back/storage/null_storage_provider.h"
#ifdef ENABLE_S3_STORAGE
#include "mail_system/back/storage/s3_storage_provider.h"
#endif
#ifdef HAVE_MARIADB
#include "mail_system/back/db/mariadb_pool.h"
#endif
#include <functional>
#include <stdexcept>

namespace mail_system {
namespace bootstrap {

// 前置声明（实现见下）：主 DB 池工厂，从原 pr::ServerBase 构造函数原样搬来
static std::shared_ptr<pr::DBPool> build_main_db_pool(const ServerConfig& config);
static std::shared_ptr<pr::storage::IStorageProvider> build_main_storage(const ServerConfig& config);

Assembled assemble(const ServerConfig& config, const Options& opts) {
    Assembled out;

    pr::Logger::get_instance().init(config.log_file, 5 * 1024 * 1024, 3,
        pr::Logger::string_to_level(config.log_level), config.log_to_console, config.log_to_file);

    // ---- 1. 线程池 ----
    out.io_pool = opts.io_pool;
    if (!out.io_pool) {
        if (config.io_thread_count <= 0)
            throw std::runtime_error("bootstrap: io_thread_count must be > 0");
        out.io_pool = std::make_shared<pr::IOThreadPool>(
            static_cast<size_t>(config.io_thread_count));
        out.io_pool->start();
    }
    out.worker_pool = opts.worker_pool;
    if (!out.worker_pool) {
        if (config.worker_thread_count <= 0)
            throw std::runtime_error("bootstrap: worker_thread_count must be > 0");
        out.worker_pool = std::make_shared<pr::WorkerThreadPool>(
            static_cast<size_t>(config.worker_thread_count));
        out.worker_pool->start();
    }
    LOG_SERVER_INFO("Thread pools ready (io={} worker={})",
                    out.io_pool->thread_count(), out.worker_pool->thread_count());

    // ---- 2. 主 DB 池 ----
    auto main_db_pool = build_main_db_pool(config);
    if (!main_db_pool) {
        main_db_pool = std::make_shared<NullDBPool>();
        LOG_SERVER_INFO("Null database pool created");
    } else {
        LOG_SERVER_INFO("Database pool initialized successfully");
    }

    // ---- 3. 主存储 ----
    auto main_storage = build_main_storage(config);
    {
        pr::storage::IoError err;
        if (!main_storage->ensure_ready(err))
            throw std::runtime_error("Storage init failed: " + err.message);
    }

    // ---- 3b. 远程后端套真异步装饰器 ----
    // S3/WebHDFS 的 PUT/GET 是毫秒~百毫秒级阻塞网络操作：commit_async 与三个 async
    // 读投递到 worker 线程池执行，io 线程不再被卡（调用点已按 pause 独占约定改造）。
    // 本地后端保持内联——stat/mmap/进 page cache 的写都是 µs 级，多一次线程跳转纯属浪费。
    // 纯 DI 红利：装配此刻 worker 池已就绪，直接捕获 shared_ptr，
    // 不再需要原 pr::ServerBase 版本的"惰性取池"间接层。
    const auto& sc = config.storage;
    if (sc.provider == "s3" || sc.provider == "hdfs_web") {
        auto worker = out.worker_pool;
        main_storage = std::make_shared<storage::AsyncStorageProvider>(
            main_storage,
            [worker = std::move(worker)](std::function<void()> task) {
                if (worker) worker->post(std::move(task));
                else task();
            });
    }

    // ---- 4. Shard Router ----
    auto& rc = config.router_config;
    std::vector<std::shared_ptr<pr::DBPool>> shard_db_pools;
    std::vector<std::shared_ptr<pr::storage::IStorageProvider>> shard_storages;

    if (!rc.shards.empty()) {
        for (size_t i = 0; i < rc.shards.size() && i < rc.shard_count; ++i) {
            auto& se = rc.shards[i];
            std::shared_ptr<pr::DBPool> sdb;
            if (!se.db_config_file.empty()) {
                pr::DBPoolConfig scfg;
                if (scfg.loadFromJson(se.db_config_file)) {
                    if (scfg.achieve == "mysql_distributed")
                        sdb = DistributedMySQLPoolFactory::get_instance().create_pool(
                            scfg, std::make_shared<MySQLService>());
                    else
                        sdb = MySQLPoolFactory::get_instance().create_pool(
                            scfg, std::make_shared<MySQLService>());
                }
            }
            shard_db_pools.push_back(sdb ? sdb : main_db_pool);

            std::shared_ptr<pr::storage::IStorageProvider> sst;
            if (!se.storage_root.empty()) {
                auto mail_dir = se.storage_root + "/mail";
                auto att_dir  = se.storage_root + "/attachments";
                if (!std::filesystem::exists(mail_dir))
                    std::filesystem::create_directories(mail_dir);
                if (!std::filesystem::exists(att_dir))
                    std::filesystem::create_directories(att_dir);
                sst = std::make_shared<storage::LocalFileStorageProvider>(mail_dir, att_dir);
            }
            shard_storages.push_back(sst ? sst : main_storage);
        }
    } else {
        shard_db_pools = {main_db_pool};
        shard_storages = {main_storage};
    }

    if (rc.type == "table") {
        out.shard_router = std::make_shared<router::TableShardRouter>(
            main_db_pool,
            rc.table_name, rc.email_column, rc.shard_column,
            rc.shard_count, rc.cache_capacity,
            shard_db_pools, shard_storages);
    } else if (rc.type == "static") {
        out.shard_router = std::make_shared<router::StaticShardRouter>(
            rc.static_mappings, rc.default_shard,
            shard_db_pools, shard_storages);
    } else {
        out.shard_router = std::make_shared<router::HashShardRouter>(
            rc.shard_count, shard_db_pools, shard_storages);
    }
    LOG_SERVER_INFO("Shard router initialized: type={} shard_count={}",
                    out.shard_router->name(), out.shard_router->shard_count());

    return out;
}

static std::shared_ptr<pr::DBPool> build_main_db_pool(const ServerConfig& config) {
    if (!config.use_database) return nullptr;
    if (config.db_pool_config.achieve.find("mysql") == 0) {
        try {
            if (config.db_pool_config.achieve == "mysql_distributed")
                return DistributedMySQLPoolFactory::get_instance().create_pool(
                    config.db_pool_config, std::make_shared<MySQLService>());
            return MySQLPoolFactory::get_instance().create_pool(
                config.db_pool_config, std::make_shared<MySQLService>());
        } catch (const std::exception& e) {
            LOG_SERVER_ERROR("Failed to create MySQL pool: {}", e.what());
            return nullptr;
        }
    }
#ifdef HAVE_MARIADB
    if (config.db_pool_config.achieve == "mariadb") {
        try {
            // db_service 传 nullptr：工厂内部创建 MariaDBService
            return MariaDBPoolFactory::get_instance().create_pool(
                config.db_pool_config, nullptr);
        } catch (const std::exception& e) {
            LOG_SERVER_ERROR("Failed to create MariaDB pool: {}", e.what());
            return nullptr;
        }
    }
#else
    if (config.db_pool_config.achieve == "mariadb") {
        LOG_SERVER_ERROR("achieve=mariadb 但本构建未启用 MariaDB 引擎（缺 mariadb-connector-c 头文件）");
        return nullptr;
    }
#endif
    return nullptr;
}

static std::shared_ptr<pr::storage::IStorageProvider> build_main_storage(const ServerConfig& config) {
    const auto& sc = config.storage;
    switch (pr::storage::provider_type_from_string(sc.provider)) {
    case pr::storage::StorageProviderType::Distributed:
        return std::make_shared<storage::DistributedFileStorageProvider>(
            sc.distributed.roots, sc.distributed.replica_count);
    case pr::storage::StorageProviderType::HdfsWeb:
#if PROTORELAY_ENABLE_HDFS_WEB_STORAGE
        return std::make_shared<storage::HdfsWebStorageProvider>(
            sc.hdfs.endpoint, sc.hdfs.base_path, sc.hdfs.user,
            sc.hdfs.replication, static_cast<long>(sc.hdfs.timeout_ms),
            sc.hdfs.max_write_buffer_bytes);
#else
        throw std::runtime_error("hdfs_web requires ENABLE_HDFS_WEB_STORAGE=ON");
#endif
    case pr::storage::StorageProviderType::Null:
        return std::make_shared<storage::NullStorageProvider>();
    case pr::storage::StorageProviderType::S3:
#ifdef ENABLE_S3_STORAGE
        return std::make_shared<storage::S3StorageProvider>(
            sc.s3.endpoint, sc.s3.bucket,
            sc.s3.access_key, sc.s3.secret_key,
            sc.s3.region, static_cast<long>(sc.s3.timeout_ms),
            sc.s3.use_path_style, sc.s3.max_write_buffer_bytes);
#else
        throw std::runtime_error("s3 requires ENABLE_S3_STORAGE=ON");
#endif
    case pr::storage::StorageProviderType::Local:
    default:
        if (!std::filesystem::exists(sc.local.mail_path))
            std::filesystem::create_directories(sc.local.mail_path);
        if (!std::filesystem::exists(sc.local.attachment_path))
            std::filesystem::create_directories(sc.local.attachment_path);
        return std::make_shared<storage::LocalFileStorageProvider>(
            sc.local.mail_path, sc.local.attachment_path);
    }
}

} // namespace bootstrap
} // namespace mail_system
