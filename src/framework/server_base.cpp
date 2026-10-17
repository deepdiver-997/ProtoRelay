// ─────────────────────────────────────────────────────────────────
// ServerBase —— 纯依赖注入（2026-10-18）。
//
// 构造函数只接受现成资源（io 池 / worker 池 / shard router），空指针响亮
// 抛 invalid_argument，绝不悄悄自建。装配（DB 池/存储/路由的具体工厂）在
// mail_system/back/bootstrap.cpp —— 框架不再认识任何具体驱动，依赖方向
// 恢复"应用 → 框架"。本文件保留的唯一应用层 include 是 i_shard_router.h：
// refresh_metrics 要遍历 router 报告各 shard 的池/存储状态（生命周期职责，
// 非装配职责）。
// ─────────────────────────────────────────────────────────────────
#include "framework/server_base.h"
#include "framework/thread_pool/io_thread_pool.h"
#include "framework/thread_pool/boost_thread_pool.h"
#include "framework/log/log_macros.h"
#include "mail_system/back/router/i_shard_router.h"
#include <iostream>
#include <fstream>
#include <stdexcept>

namespace pr {

ServerBase::ServerBase(const mail_system::ServerConfig& config,
     std::shared_ptr<ThreadPoolBase> ioThreadPool,
      std::shared_ptr<ThreadPoolBase> workerThreadPool,
       std::shared_ptr<mail_system::router::IShardRouter> shardRouter)
    : m_ioThreadPool(std::move(ioThreadPool)),
      m_workerThreadPool(std::move(workerThreadPool)),
      m_shardRouter(std::move(shardRouter)),
      m_domain(config.system_domain.empty() ? std::string("example.com") : config.system_domain),
      m_config(std::make_shared<mail_system::ServerConfig>(config))
{
    // 纯 DI：资源必须齐备。静默容忍空指针曾让"谁负责建池"这笔账糊涂了三年
    // （构造函数里藏着一个 180 行的工厂，测试只能靠事后覆盖 m_shardRouter 绕开）。
    if (!m_ioThreadPool)
        throw std::invalid_argument("ServerBase: io thread pool is required "
                                    "(assemble via mail_system::bootstrap::assemble)");
    if (!m_workerThreadPool)
        throw std::invalid_argument("ServerBase: worker thread pool is required "
                                    "(assemble via mail_system::bootstrap::assemble)");
    if (!m_shardRouter)
        throw std::invalid_argument("ServerBase: shard router is required "
                                    "(assemble via mail_system::bootstrap::assemble)");
    if (!m_ioThreadPool->is_running() || !m_workerThreadPool->is_running())
        throw std::invalid_argument("ServerBase: thread pools must be started before construction");

    try {
        Logger::get_instance().init(
            config.log_file, 1024 * 1024 * 5, 3,
            Logger::string_to_level(config.log_level),
            config.log_to_console, config.log_to_file);
        LOG_SERVER_INFO("Logger initialized with level: {}", config.log_level);
        LOG_SERVER_INFO("Server initialized with {} listener(s)", config.listeners.size());
        m_state = ServerState::Paused;
    } catch (const std::exception& e) {
        LOG_SERVER_ERROR("Error initializing server: {}", e.what());
        throw;
    }
}

ServerBase::~ServerBase() {
    try { stop(); } catch (...) {}
}

void ServerBase::run() {
    while (m_state.load() == ServerState::Running || m_state.load() == ServerState::Paused)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void ServerBase::stop(ServerState state) {
    auto cur = m_state.load();
    if (cur != ServerState::Running && cur != ServerState::Pausing) return;
    m_state.store(state);

    stop_metrics_server();

    // 纯 DI（2026-10-18）：io/worker 池是调用方注入的资源，所有权在装配方
    // （bootstrap / main / 测试 fixture），ServerBase 析构与 stop() 都不碰它们
    // ——共享注入场景（mailServer 合并入口两 server 共池、测试多用例共池）下，
    // 第一个 server 析构就把池停掉，后续构造直接拿到"已停止"的池。
    // 池的关闭顺序归装配方：main 持 Assembled 资源，作用域结束时自行 stop。

    m_intrusionDetector.persist();
    LOG_SERVER_INFO("Server stopped");
}

ServerState ServerBase::get_state() const { return m_state.load(); }

void ServerBase::start_metrics_server() {
    auto cfg = std::atomic_load(&m_config);
    if (!cfg->metrics_enabled || m_metricsServer) return;
    try {
        auto& io_ctx = std::static_pointer_cast<IOThreadPool>(m_ioThreadPool)->get_io_context();
        m_metricsServer = std::make_shared<MetricsServer>(
            io_ctx,
            cfg->metrics_port, cfg->metrics_bind_address,
            [this]() -> std::string {
                if (m_configFilePath.empty()) return "config file path not set";
                return reload_config(m_configFilePath) ? "OK" : "reload failed";
            },
            [this]() { refresh_metrics(); });
        m_metricsServer->start();
        refresh_metrics();
        LOG_SERVER_INFO("Metrics server started on {}:{}",
                        cfg->metrics_bind_address, cfg->metrics_port);
    } catch (const std::exception& e) {
        LOG_SERVER_ERROR("Failed to start metrics server: {}", e.what());
        m_metricsServer.reset();
    }
}

void ServerBase::stop_metrics_server() {
    if (m_metricsServer) { m_metricsServer->stop(); m_metricsServer.reset(); }
}

void ServerBase::push_metric_gauge(const std::string& name,
                                     const MetricsServer::LabelMap& labels, double v) {
    if (auto m = m_metricsServer) m->set_gauge(name, labels, v);
}
void ServerBase::push_metric_counter(const std::string& name,
                                       const MetricsServer::LabelMap& labels, uint64_t v) {
    if (auto m = m_metricsServer) m->inc_counter(name, labels, v);
}
void ServerBase::push_metric_observe(const std::string& name,
                                       const MetricsServer::LabelMap& labels, double v) {
    if (auto m = m_metricsServer) m->observe(name, labels, v);
}

void ServerBase::increment_connection_count() {
    active_connections_.fetch_add(1, std::memory_order_relaxed);
    push_metric_gauge("protorelay_active_connections", {}, active_connections_.load());
}
void ServerBase::decrement_connection_count() {
    active_connections_.fetch_sub(1, std::memory_order_relaxed);
    push_metric_gauge("protorelay_active_connections", {}, active_connections_.load());
}
// 2026-08-27 fix: 之前把 fetch_add 返回的累计值 v 当 delta 传给 inc_counter
// （c.value += delta），结果 N 次调用后 counter map 累加 1+2+...+N = N*(N+1)/2
// （三角形数），不是 N。inc_counter 语义是"additive delta"，应传 1。
// 修法：fetch_add 后传 delta=1，保留 ServerBase 自己的 atomic 累计值供其他用途。
void ServerBase::increment_connections_total() {
    connections_total_.fetch_add(1, std::memory_order_relaxed);
    push_metric_counter("protorelay_connections_total", {}, 1);
}
void ServerBase::increment_connections_rejected() {
    connections_rejected_total_.fetch_add(1, std::memory_order_relaxed);
    push_metric_counter("protorelay_connections_rejected_total", {}, 1);
}
void ServerBase::increment_mails_accepted() {
    mails_accepted_total_.fetch_add(1, std::memory_order_relaxed);
    push_metric_counter("protorelay_mails_accepted_total", {}, 1);
}

void ServerBase::refresh_metrics() {
    if (!m_metricsServer) return;
    if (m_shardRouter) {
        size_t n = m_shardRouter->shard_count();
        for (size_t i = 0; i < n; ++i) {
            MetricsServer::LabelMap labels{{"shard", std::to_string(i)}};
            auto db = m_shardRouter->get_db_pool(i);
            if (db) {
                m_metricsServer->set_gauge("protorelay_db_pool_size", labels, db->get_pool_size());
                m_metricsServer->set_gauge("protorelay_db_available", labels, db->get_available_connections());
                m_metricsServer->set_gauge("protorelay_db_active", labels, db->get_active_connections());
                m_metricsServer->set_gauge("protorelay_db_pool_max", labels, db->get_max_pool_size());
            }
            auto st = m_shardRouter->get_storage(i);
            m_metricsServer->set_gauge("protorelay_storage_ready", labels, st ? 1 : 0);
        }
    }
}

bool ServerBase::reload_config(const std::string& json_file) {
    m_configFilePath = json_file;
    mail_system::ServerConfig new_cfg = *std::atomic_load(&m_config);
    if (!new_cfg.loadFromFile(json_file)) {
        LOG_SERVER_ERROR("Config reload failed: could not parse {}", json_file);
        return false;
    }
    auto old_cfg = std::atomic_load(&m_config);
    if (old_cfg->address != new_cfg.address ||
        old_cfg->listeners.size() != new_cfg.listeners.size()) {
        LOG_SERVER_WARN("Config reload rejected: structural fields changed (requires restart)");
        return false;
    }
    for (size_t i = 0; i < old_cfg->listeners.size(); ++i) {
        if (old_cfg->listeners[i].port != new_cfg.listeners[i].port ||
            old_cfg->listeners[i].type != new_cfg.listeners[i].type) {
            LOG_SERVER_WARN("Config reload rejected: listener structure changed");
            return false;
        }
    }
    std::atomic_store(&m_config, std::make_shared<mail_system::ServerConfig>(new_cfg));
    auto applied = std::atomic_load(&m_config);
    Logger::get_instance().set_level(Logger::string_to_level(applied->log_level));
    LOG_SERVER_INFO("Config reloaded: log_level={}, auth={}",
                    applied->log_level, mail_system::inbound_auth_policy_to_string(applied->inbound_auth_policy));
    return true;
}

// deprecated stubs
std::string ServerBase::build_metrics_response() const { return ""; }
std::string ServerBase::build_status_response() const { return ""; }

} // namespace pr
