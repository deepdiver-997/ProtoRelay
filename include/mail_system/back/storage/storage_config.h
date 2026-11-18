#ifndef MAIL_SYSTEM_STORAGE_CONFIG_H
#define MAIL_SYSTEM_STORAGE_CONFIG_H

#include "framework/storage/local_storage_runtime.h"
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace mail_system {
namespace storage {

// === 各后端的专属配置 ===
struct LocalStorageConfig {
    std::string mail_path;
    std::string attachment_path;
    // Startup-only, shared by the main local provider and all local shards.
    pr::LocalStorageRuntimeConfig runtime_config;
};

struct S3StorageConfig {
    std::string endpoint = "http://127.0.0.1:9000";
    std::string bucket   = "protorelay";
    std::string access_key = "minioadmin";
    std::string secret_key = "minioadmin";
    std::string region     = "us-east-1";
    uint32_t timeout_ms    = 5000;
    bool use_path_style    = true;
    // 流式写入的整对象内存缓冲上限（open_write 攒到 commit 一次上传）。
    uint64_t max_write_buffer_bytes = 64ull * 1024 * 1024;
};

struct HdfsStorageConfig {
    std::string endpoint    = "http://127.0.0.1:9870";
    std::string base_path   = "/mail-system";
    std::string user        = "hdfs";
    uint32_t timeout_ms     = 5000;
    size_t replication      = 1;
    // 同 S3StorageConfig::max_write_buffer_bytes。
    uint64_t max_write_buffer_bytes = 64ull * 1024 * 1024;
};

struct DistributedStorageConfig {
    std::vector<std::string> roots;
    size_t replica_count = 1;
};

// === 统一存储配置 ===
enum class StorageProviderType { Local, Null, S3, HdfsWeb, Distributed };

inline StorageProviderType provider_type_from_string(const std::string& s) {
    if (s == "distributed") return StorageProviderType::Distributed;
    if (s == "hdfs_web")    return StorageProviderType::HdfsWeb;
    if (s == "null")        return StorageProviderType::Null;
    if (s == "s3")          return StorageProviderType::S3;
    return StorageProviderType::Local;
}

struct StorageConfig {
    std::string provider = "local";  // "local" | "null" | "s3" | "hdfs_web" | "distributed"

    LocalStorageConfig local;
    S3StorageConfig s3;
    HdfsStorageConfig hdfs;
    DistributedStorageConfig distributed;

    static StorageConfig from_json(const nlohmann::json& j, const std::string& base_dir = "") {
        StorageConfig cfg;
        cfg.provider = j.value("provider", cfg.provider);

        auto resolve = [&](const std::string& path) {
            if (path.empty() || path[0] == '/' || base_dir.empty()) return path;
            return base_dir + "/" + path;
        };

        if (j.contains("local")) {
            auto& l = j["local"];
            cfg.local.mail_path       = resolve(l.value("mail_path", ""));
            cfg.local.attachment_path = resolve(l.value("attachment_path", ""));
            auto& runtime = cfg.local.runtime_config;
            const auto mode = l.value("commit_mode", std::string("offload"));
            if (mode == "offload") runtime.commit_mode = pr::LocalCommitMode::Offload;
            else if (mode == "inline") runtime.commit_mode = pr::LocalCommitMode::Inline;
            else throw std::invalid_argument("storage.local.commit_mode must be 'inline' or 'offload'");

            // json::get<unsigned>() silently wraps negative integers. Validate the
            // JSON type and range before conversion, including size_t on 32-bit.
            auto positive_limit = [&](const char* field, uint64_t fallback, uint64_t maximum) {
                if (!l.contains(field)) return fallback;
                const auto& value = l.at(field);
                if (!value.is_number_integer() ||
                    (!value.is_number_unsigned() && value.get<int64_t>() <= 0))
                    throw std::invalid_argument(std::string("storage.local.") + field +
                                                " must be a positive integer");
                const auto number = value.get<uint64_t>();
                if (number == 0 || number > maximum)
                    throw std::invalid_argument(std::string("storage.local.") + field +
                                                " is out of range");
                return number;
            };
            runtime.commit_threads = static_cast<size_t>(positive_limit(
                "commit_threads", runtime.commit_threads, std::numeric_limits<size_t>::max()));
            runtime.max_pending_commits = static_cast<size_t>(positive_limit(
                "max_pending_commits", runtime.max_pending_commits, std::numeric_limits<size_t>::max()));
            runtime.max_pending_commit_bytes = positive_limit(
                "max_pending_commit_bytes", runtime.max_pending_commit_bytes,
                std::numeric_limits<uint64_t>::max());
            runtime.validate();
        }

        if (j.contains("s3")) {
            auto& s = j["s3"];
            cfg.s3.endpoint       = s.value("endpoint", cfg.s3.endpoint);
            cfg.s3.bucket         = s.value("bucket", cfg.s3.bucket);
            cfg.s3.access_key     = s.value("access_key", cfg.s3.access_key);
            cfg.s3.secret_key     = s.value("secret_key", cfg.s3.secret_key);
            cfg.s3.region         = s.value("region", cfg.s3.region);
            cfg.s3.timeout_ms     = s.value("timeout_ms", cfg.s3.timeout_ms);
            cfg.s3.use_path_style = s.value("use_path_style", cfg.s3.use_path_style);
            cfg.s3.max_write_buffer_bytes = s.value("max_write_buffer_bytes",
                                                    cfg.s3.max_write_buffer_bytes);
        }

        if (j.contains("hdfs")) {
            auto& h = j["hdfs"];
            cfg.hdfs.endpoint    = h.value("endpoint", cfg.hdfs.endpoint);
            cfg.hdfs.base_path   = h.value("base_path", cfg.hdfs.base_path);
            cfg.hdfs.user        = h.value("user", cfg.hdfs.user);
            cfg.hdfs.timeout_ms  = h.value("timeout_ms", cfg.hdfs.timeout_ms);
            cfg.hdfs.replication = h.value("replication", cfg.hdfs.replication);
            cfg.hdfs.max_write_buffer_bytes = h.value("max_write_buffer_bytes",
                                                      cfg.hdfs.max_write_buffer_bytes);
        }

        if (j.contains("distributed")) {
            auto& d = j["distributed"];
            cfg.distributed.replica_count = d.value("replica_count", cfg.distributed.replica_count);
            if (d.contains("roots") && d["roots"].is_array()) {
                for (auto& r : d["roots"]) {
                    std::string p = resolve(r.get<std::string>());
                    if (!p.empty()) cfg.distributed.roots.push_back(p);
                }
            }
        }

        return cfg;
    }
};

} // namespace storage
} // namespace mail_system
#endif
