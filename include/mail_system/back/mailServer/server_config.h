#ifndef MAIL_SYSTEM_SERVER_CONFIG_H
#define MAIL_SYSTEM_SERVER_CONFIG_H

#include <thread>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include "mail_system/back/db/db_pool.h"

namespace mail_system {

enum class InboundAckMode : int { AFTER_PERSIST = 0, AFTER_ENQUEUE = 1 };

inline const char* inbound_ack_mode_to_string(InboundAckMode mode) {
    switch (mode) {
    case InboundAckMode::AFTER_ENQUEUE: return "after_enqueue";
    case InboundAckMode::AFTER_PERSIST:
    default: return "after_persist";
    }
}

inline InboundAckMode inbound_ack_mode_from_string(const std::string& s) {
    if (s == "after_enqueue") return InboundAckMode::AFTER_ENQUEUE;
    return InboundAckMode::AFTER_PERSIST;
}

enum class InboundAuthPolicy : int { OFF = 0, AUTO = 1, ON = 2 };

inline const char* inbound_auth_policy_to_string(InboundAuthPolicy p) {
    switch (p) {
    case InboundAuthPolicy::AUTO: return "auto";
    case InboundAuthPolicy::ON:   return "on";
    case InboundAuthPolicy::OFF:
    default: return "off";
    }
}

inline InboundAuthPolicy inbound_auth_policy_from_string(const std::string& s) {
    if (s == "auto") return InboundAuthPolicy::AUTO;
    if (s == "on")   return InboundAuthPolicy::ON;
    return InboundAuthPolicy::OFF;
}

inline const char* spf_mode_validate(const std::string& s) {
    if (s == "off" || s == "soft" || s == "hard") return s.c_str();
    return "off";
}

// ============================================================
// 单个监听器配置
// ============================================================
enum class ListenerType : int { TCP = 0, SSL = 1 };

inline const char* listener_type_to_string(ListenerType t) {
    switch (t) {
    case ListenerType::SSL: return "ssl";
    case ListenerType::TCP:
    default: return "tcp";
    }
}

inline ListenerType listener_type_from_string(const std::string& s) {
    if (s == "ssl") return ListenerType::SSL;
    return ListenerType::TCP;
}

struct ListenerConfig {
    ListenerType type = ListenerType::TCP;
    uint16_t port = 0;
    InboundAuthPolicy auth_policy = InboundAuthPolicy::OFF;
    std::string spf_mode   = "off";
    std::string dkim_mode  = "off";
    std::string dmarc_mode = "off";

    void show() const {
        std::cout << "  [" << listener_type_to_string(type)
                  << ":" << port << "] auth=" << inbound_auth_policy_to_string(auth_policy)
                  << " spf=" << spf_mode
                  << " dkim=" << dkim_mode
                  << " dmarc=" << dmarc_mode << std::endl;
    }
};

// ============================================================
// 服务器配置
// ============================================================
struct ServerConfig {
    std::string address;

    // 多监听器配置
    std::vector<ListenerConfig> listeners;

    // SSL 证书
    std::string certFile;
    std::string keyFile;
    std::string dhFile;

    size_t maxMessageSize;
    size_t maxConnections;

    size_t io_thread_count;
    size_t worker_thread_count;

    bool use_database;
    DBPoolConfig db_pool_config;

    uint32_t connection_timeout;
    uint32_t read_timeout;
    uint32_t write_timeout;

    size_t max_auth_attempts;
    bool dnsbl_enabled;

    std::string log_level;
    std::string log_file;
    bool log_to_console;
    bool log_to_file;

    std::string storage_provider;
    std::string mail_storage_path;
    std::string attachment_storage_path;
    std::vector<std::string> distributed_storage_roots;
    size_t distributed_storage_replica_count;
    std::string hdfs_endpoint;
    std::string hdfs_base_path;
    std::string hdfs_user;
    uint32_t hdfs_timeout_ms;
    size_t hdfs_replication;

    std::string system_name;
    std::string system_domain;
    std::vector<uint16_t> outbound_ports;
    size_t outbound_max_attempts;
    uint32_t outbound_poll_busy_sleep_ms;
    uint32_t outbound_poll_backoff_base_ms;
    uint32_t outbound_poll_backoff_max_ms;
    uint32_t outbound_poll_backoff_shift_cap;

    InboundAckMode inbound_ack_mode;
    uint32_t inbound_persist_wait_timeout_ms;
    size_t persist_max_inflight_mails;
    size_t persist_min_available_memory_mb;
    size_t persist_min_db_available_connections;

    std::string outbound_helo_domain;
    std::string outbound_mail_from_domain;
    bool outbound_rewrite_header_from;
    bool outbound_dkim_enabled;
    std::string outbound_dkim_selector;
    std::string outbound_dkim_domain;
    std::string outbound_dkim_private_key_file;

    bool metrics_enabled;
    uint16_t metrics_port;
    std::string metrics_bind_address;

    // 全局默认值 — listener 未指定时回退到这些
    InboundAuthPolicy inbound_auth_policy;
    std::string inbound_spf_mode;
    std::string inbound_dkim_mode;
    std::string inbound_dmarc_mode;
    uint32_t inbound_auth_timeout_ms;

    bool intrusion_detection_enabled;
    int  intrusion_persist_interval_sec;
    int  intrusion_persist_dirty_threshold;
    int  intrusion_max_records;
    int  intrusion_ban_threshold;

    // ---- 便捷方法：按端口查 listener 配置 ----
    std::unordered_map<uint16_t, ListenerConfig> listener_map() const {
        std::unordered_map<uint16_t, ListenerConfig> m;
        for (auto& l : listeners) m[l.port] = l;
        return m;
    }

    const ListenerConfig* find_listener(uint16_t port) const {
        for (auto& l : listeners) if (l.port == port) return &l;
        return nullptr;
    }

    ServerConfig()
        : address("0.0.0.0")
        , maxMessageSize(1024 * 1024)
        , maxConnections(1000)
        , io_thread_count(std::thread::hardware_concurrency())
        , worker_thread_count(std::thread::hardware_concurrency())
        , use_database(false)
        , connection_timeout(300)
        , read_timeout(60)
        , write_timeout(60)
        , max_auth_attempts(3)
        , dnsbl_enabled(true)
        , log_level("info")
        , log_to_console(true)
        , log_to_file(true)
        , storage_provider("local")
        , distributed_storage_replica_count(1)
        , hdfs_endpoint("http://127.0.0.1:9870")
        , hdfs_base_path("/mail-system")
        , hdfs_user("hdfs")
        , hdfs_timeout_ms(5000)
        , hdfs_replication(1)
        , system_name("mail-system")
        , system_domain("example.com")
        , outbound_ports({25, 587, 465})
        , outbound_max_attempts(8)
        , outbound_poll_busy_sleep_ms(20)
        , outbound_poll_backoff_base_ms(50)
        , outbound_poll_backoff_max_ms(1200)
        , outbound_poll_backoff_shift_cap(6)
        , inbound_ack_mode(InboundAckMode::AFTER_PERSIST)
        , inbound_persist_wait_timeout_ms(5000)
        , persist_max_inflight_mails(2048)
        , persist_min_available_memory_mb(256)
        , persist_min_db_available_connections(1)
        , outbound_helo_domain("outbound.local")
        , outbound_rewrite_header_from(true)
        , outbound_dkim_enabled(false)
        , outbound_dkim_selector("default")
        , metrics_enabled(false)
        , metrics_port(9090)
        , metrics_bind_address("127.0.0.1")
        , inbound_auth_policy(InboundAuthPolicy::OFF)
        , inbound_spf_mode("off")
        , inbound_dkim_mode("off")
        , inbound_dmarc_mode("off")
        , inbound_auth_timeout_ms(30000)
        , intrusion_detection_enabled(false)
        , intrusion_persist_interval_sec(60)
        , intrusion_persist_dirty_threshold(256)
        , intrusion_max_records(10000)
        , intrusion_ban_threshold(0)
    {}

    ServerConfig(const ServerConfig&) = default;

    void show() const {
        std::cout << "\naddress = " << address
                  << "\nio_thread_count = " << io_thread_count
                  << "\nworker_thread_count = " << worker_thread_count
                  << "\nmaxConnections = " << maxConnections
                  << "\nuse_database = " << (use_database ? "true" : "false")
                  << "\nsystem_domain = " << system_domain;

        std::cout << "\n\nlisteners:";
        for (auto& l : listeners) l.show();

        std::cout << "\nSSL certs: cert=" << (certFile.empty() ? "(none)" : certFile)
                  << " key=" << (keyFile.empty() ? "(none)" : keyFile)
                  << "\ninbound_auth_policy = " << inbound_auth_policy_to_string(inbound_auth_policy)
                  << "\ninbound_spf_mode = " << inbound_spf_mode
                  << "\ninbound_dkim_mode = " << inbound_dkim_mode
                  << "\ninbound_dmarc_mode = " << inbound_dmarc_mode
                  << "\nintrusion_detection_enabled = " << (intrusion_detection_enabled ? "true" : "false")
                  << "\nmetrics_enabled = " << (metrics_enabled ? "true" : "false")
                  << "\noutbound_helo_domain = " << outbound_helo_domain
                  << "\noutbound_dkim_enabled = " << (outbound_dkim_enabled ? "true" : "false")
                  << std::endl;
    }

    bool validate() const {
        if (listeners.empty()) {
            std::cerr << "Error: at least one listener required" << std::endl;
            return false;
        }
        bool has_ssl = false;
        for (auto& l : listeners) {
            if (l.port == 0) { std::cerr << "Error: listener port 0" << std::endl; return false; }
            if (l.type == ListenerType::SSL) has_ssl = true;
        }
#ifdef USE_SSL
        if (has_ssl && (certFile.empty() || keyFile.empty())) {
            std::cerr << "Error: SSL listener requires certFile/keyFile" << std::endl;
            return false;
        }
#endif
        if (io_thread_count == 0 || worker_thread_count == 0) {
            std::cerr << "Error: thread pool size 0" << std::endl; return false;
        }
        if (connection_timeout == 0 || read_timeout == 0 || write_timeout == 0) {
            std::cerr << "Error: timeout 0" << std::endl; return false;
        }
        if (inbound_persist_wait_timeout_ms == 0) {
            std::cerr << "Error: inbound_persist_wait_timeout_ms 0" << std::endl; return false;
        }
        if (storage_provider == "distributed" && distributed_storage_roots.empty()) {
            std::cerr << "Error: distributed needs roots" << std::endl; return false;
        }
        return true;
    }

    std::string resolve_path(const std::string& config_path, const std::string& relative_path) {
        if (relative_path.empty()) return "";
        auto dir = std::filesystem::path(config_path).parent_path();
        return std::filesystem::absolute((dir / relative_path).lexically_normal()).string();
    }

    bool loadFromFile(const std::string& filename) {
        std::ifstream config_file(filename);
        if (!config_file.is_open()) {
            std::cerr << "Failed to open config: " << filename << std::endl;
            return false;
        }
        nlohmann::json j;
        config_file >> j;

        // --- listeners ---
        if (j.contains("listeners") && j["listeners"].is_array()) {
            listeners.clear();
            for (auto& item : j["listeners"]) {
                ListenerConfig lc;
                lc.type   = listener_type_from_string(item.value("type", "tcp"));
                lc.port   = static_cast<uint16_t>(item.value("port", 0));
                lc.auth_policy = inbound_auth_policy_from_string(
                    item.value("auth_policy", "off"));
                lc.spf_mode   = item.value("spf_mode", "off");
                lc.dkim_mode  = item.value("dkim_mode", "off");
                lc.dmarc_mode = item.value("dmarc_mode", "off");
                if (lc.port != 0) listeners.push_back(lc);
            }
        }

        // --- common fields ---
        address           = j.value("address", address);
        maxMessageSize    = j.value("maxMessageSize", maxMessageSize);
        maxConnections    = j.value("maxConnections", maxConnections);
        io_thread_count   = j.value("io_thread_count", io_thread_count);
        worker_thread_count = j.value("worker_thread_count", worker_thread_count);
        use_database      = j.value("use_database", use_database);
        if (use_database) {
            std::string db_file = resolve_path(filename, j.value("db_config_file", ""));
            db_pool_config.loadFromJson(db_file);
        }
        connection_timeout = j.value("connection_timeout", connection_timeout);
        read_timeout       = j.value("read_timeout", read_timeout);
        write_timeout      = j.value("write_timeout", write_timeout);
        max_auth_attempts  = j.value("max_auth_attempts", max_auth_attempts);
        dnsbl_enabled      = j.value("dnsbl_enabled", dnsbl_enabled);
        log_level          = j.value("log_level", log_level);
        log_file           = resolve_path(filename, j.value("log_file", log_file));
        log_to_console     = j.value("log_to_console", log_to_console);
        log_to_file        = j.value("log_to_file", log_to_file);
        storage_provider   = j.value("storage_provider", storage_provider);
        mail_storage_path  = resolve_path(filename, j.value("mail_storage_path", mail_storage_path));
        attachment_storage_path = resolve_path(filename, j.value("attachment_storage_path", attachment_storage_path));

        system_name           = j.value("system_name", system_name);
        system_domain         = j.value("system_domain", system_domain);
        outbound_helo_domain  = j.value("outbound_helo_domain", outbound_helo_domain);
        outbound_mail_from_domain = j.value("outbound_mail_from_domain", outbound_mail_from_domain);
        outbound_rewrite_header_from = j.value("outbound_rewrite_header_from", outbound_rewrite_header_from);
        outbound_dkim_enabled = j.value("outbound_dkim_enabled", outbound_dkim_enabled);
        outbound_dkim_selector  = j.value("outbound_dkim_selector", outbound_dkim_selector);
        outbound_dkim_domain    = j.value("outbound_dkim_domain", outbound_dkim_domain);
        outbound_dkim_private_key_file = resolve_path(filename,
            j.value("outbound_dkim_private_key_file", outbound_dkim_private_key_file));
        outbound_max_attempts  = j.value("outbound_max_attempts", outbound_max_attempts);
        outbound_poll_busy_sleep_ms  = j.value("outbound_poll_busy_sleep_ms", outbound_poll_busy_sleep_ms);
        outbound_poll_backoff_base_ms= j.value("outbound_poll_backoff_base_ms", outbound_poll_backoff_base_ms);
        outbound_poll_backoff_max_ms = j.value("outbound_poll_backoff_max_ms", outbound_poll_backoff_max_ms);
        outbound_poll_backoff_shift_cap= j.value("outbound_poll_backoff_shift_cap", outbound_poll_backoff_shift_cap);

        inbound_ack_mode = inbound_ack_mode_from_string(
            j.value("inbound_ack_mode", std::string(inbound_ack_mode_to_string(inbound_ack_mode))));
        inbound_persist_wait_timeout_ms = j.value("inbound_persist_wait_timeout_ms", inbound_persist_wait_timeout_ms);
        persist_max_inflight_mails      = j.value("persist_max_inflight_mails", persist_max_inflight_mails);
        persist_min_available_memory_mb = j.value("persist_min_available_memory_mb", persist_min_available_memory_mb);
        persist_min_db_available_connections = j.value("persist_min_db_available_connections", persist_min_db_available_connections);

        inbound_auth_policy = inbound_auth_policy_from_string(
            j.value("inbound_auth_policy", std::string(inbound_auth_policy_to_string(inbound_auth_policy))));
        inbound_spf_mode   = j.value("inbound_spf_mode", inbound_spf_mode);
        inbound_dkim_mode  = j.value("inbound_dkim_mode", inbound_dkim_mode);
        inbound_dmarc_mode = j.value("inbound_dmarc_mode", inbound_dmarc_mode);
        inbound_auth_timeout_ms = j.value("inbound_auth_timeout_ms", inbound_auth_timeout_ms);

        metrics_enabled    = j.value("metrics_enabled", metrics_enabled);
        metrics_port       = j.value("metrics_port", metrics_port);
        metrics_bind_address = j.value("metrics_bind_address", metrics_bind_address);

        intrusion_detection_enabled      = j.value("intrusion_detection_enabled", false);
        intrusion_persist_interval_sec   = j.value("intrusion_persist_interval_sec", 60);
        intrusion_persist_dirty_threshold= j.value("intrusion_persist_dirty_threshold", 256);
        intrusion_max_records            = j.value("intrusion_max_records", 10000);
        intrusion_ban_threshold          = j.value("intrusion_ban_threshold", 0);

        // SSL certs
        certFile = resolve_path(filename, j.value("certFile", certFile));
        keyFile  = resolve_path(filename, j.value("keyFile", keyFile));
        dhFile   = resolve_path(filename, j.value("dhFile", dhFile));

        // outbound_ports
        if (j.contains("outbound_ports") && j["outbound_ports"].is_array()) {
            outbound_ports.clear();
            for (auto& p : j["outbound_ports"])
                if (p.is_number_unsigned() && p.get<uint32_t>() <= 65535)
                    outbound_ports.push_back(static_cast<uint16_t>(p.get<uint32_t>()));
        }

        // distributed_storage_roots
        if (j.contains("distributed_storage_roots") && j["distributed_storage_roots"].is_array()) {
            distributed_storage_roots.clear();
            for (auto& item : j["distributed_storage_roots"]) {
                if (!item.is_string()) continue;
                auto p = resolve_path(filename, item.get<std::string>());
                if (!p.empty()) distributed_storage_roots.push_back(p);
            }
        }
        distributed_storage_replica_count = j.value("distributed_storage_replica_count", distributed_storage_replica_count);
        hdfs_endpoint   = j.value("hdfs_endpoint", hdfs_endpoint);
        hdfs_base_path  = j.value("hdfs_base_path", hdfs_base_path);
        hdfs_user       = j.value("hdfs_user", hdfs_user);
        hdfs_timeout_ms = j.value("hdfs_timeout_ms", hdfs_timeout_ms);
        hdfs_replication= j.value("hdfs_replication", hdfs_replication);

        return true;
    }

    bool saveToFile(const std::string&) const { return true; }
};

} // namespace mail_system
#endif
