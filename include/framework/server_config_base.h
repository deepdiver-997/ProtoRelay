#ifndef PR_FRAMEWORK_SERVER_CONFIG_BASE_H
#define PR_FRAMEWORK_SERVER_CONFIG_BASE_H

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <fstream>
#include <thread>
#include <filesystem>
#include <nlohmann/json.hpp>

// ================================================================
// ServerConfig — 框架级服务器配置基类
//
// 抽象所有基于 TCP/SSL 的服务器的公共配置项：
// 地址、监听器、SSL 证书、线程数、超时、日志设置。
// 应用层通过继承扩展业务特有的配置（DB、存储、路由等）。
// ================================================================
namespace pr {

enum class ListenerType : int { TCP = 0, SSL = 1 };

inline const char* listener_type_to_string(ListenerType t) {
    return t == ListenerType::SSL ? "ssl" : "tcp";
}

inline ListenerType listener_type_from_string(const std::string& s) {
    return s == "ssl" ? ListenerType::SSL : ListenerType::TCP;
}

// InboundAuthPolicy — 监听器认证策略 (框架级, 任何多端口服务器都可能需要)
enum class InboundAuthPolicy : int { OFF = 0, AUTO = 1, ON = 2 };

struct ListenerConfig {
    ListenerType type = ListenerType::TCP;
    uint16_t port = 0;
    InboundAuthPolicy auth_policy = InboundAuthPolicy::OFF;
    // false = EHLO 不通告 STARTTLS，对端按 RFC 3207 走明文投递。
    // 背景：本服务器跨境方向 25 端口的 STARTTLS 握手会被链路中间设备注入 RST
    // （2026-09-28 抓包实锤：ClientHello 后伪造双 RST、IP id 相同），而 Gmail
    // 等 MTA 在 TLS 失败后只退避重试、不回落明文 —— 只要还通告 STARTTLS，
    // 这些来源就永远投不进来。SendGrid 类会回落明文的 MTA 不受影响。
    bool advertise_starttls = true;

    ListenerConfig() = default;
    ListenerConfig(ListenerType t, uint16_t p) : type(t), port(p) {}
};

struct ServerConfig {
    using Config = ServerConfig;
    std::string address;

    std::vector<ListenerConfig> listeners;

    std::string certFile;
    std::string keyFile;
    std::string dhFile;

    // TLS 1.3 开关：默认启用。Gmail 等现代 MTA 用 TLS 1.3 ClientHello 且无回退机制，
    // 服务器若强制 TLS 1.2 会导致 Gmail 在 STARTTLS 阶段直接 RST（投递失败）。
    bool enable_tls1_3 = true;

    size_t maxMessageSize;
    size_t maxConnections;

    size_t io_thread_count;
    size_t worker_thread_count;

    uint32_t connection_timeout;
    uint32_t read_timeout;
    uint32_t write_timeout;
    // IMAP IDLE 期间发现新邮件的轮询间隔（毫秒）。生产 smtps / imaps 是两个
    // 独立进程，进程内投递通知跨不过去，IDLE 会话靠自己的定时器查库发现新
    // 邮件；这个值就是跨进程推送的实际延迟上限。解析时钳制到 [200, 60000]。
    uint32_t idle_poll_interval_ms;

    size_t max_auth_attempts;

    std::string log_level;
    std::string log_file;
    bool log_to_console;
    bool log_to_file;

    bool metrics_enabled = false;
    uint16_t metrics_port = 9090;
    std::string metrics_bind_address = "127.0.0.1";
    bool intrusion_detection_enabled = false;
    int intrusion_persist_interval_sec = 60;
    int intrusion_persist_dirty_threshold = 256;
    int intrusion_max_records = 10000;
    int intrusion_ban_threshold = 0;

    ServerConfig()
        : address("0.0.0.0")
        , maxMessageSize(1024 * 1024)
        , maxConnections(1000)
        , io_thread_count(std::thread::hardware_concurrency())
        , worker_thread_count(std::thread::hardware_concurrency())
        , connection_timeout(300)
        , read_timeout(60)
        , write_timeout(60)
        , idle_poll_interval_ms(1000)
        , max_auth_attempts(3)
        , log_level("info")
        , log_to_console(true)
        , log_to_file(true)
    {}

    ServerConfig(const ServerConfig&) = default;
    virtual ~ServerConfig() = default;

    // ---- 便捷方法 ----
    std::unordered_map<uint16_t, ListenerConfig> listener_map() const {
        std::unordered_map<uint16_t, ListenerConfig> m;
        for (auto& l : listeners) m[l.port] = l;
        return m;
    }

    const ListenerConfig* find_listener(uint16_t port) const {
        for (auto& l : listeners) if (l.port == port) return &l;
        return nullptr;
    }

    static std::string resolve_path(const std::string& config_path,
                                    const std::string& relative_path) {
        if (relative_path.empty()) return "";
        auto dir = std::filesystem::path(config_path).parent_path();
        return std::filesystem::absolute((dir / relative_path).lexically_normal()).string();
    }

    // ---- 序列化 (子类应重写以扩展) ----
    virtual bool loadFromJson(const nlohmann::json& j, const std::string& base_dir) {
        if (j.contains("listeners") && j["listeners"].is_array()) {
            listeners.clear();
            for (auto& item : j["listeners"]) {
                ListenerConfig lc;
                lc.type = listener_type_from_string(item.value("type", "tcp"));
                lc.port = static_cast<uint16_t>(item.value("port", 0));
                lc.advertise_starttls = item.value("advertise_starttls", true);
                if (lc.port != 0) listeners.push_back(lc);
            }
        }

        address             = j.value("address", address);
        maxMessageSize      = j.value("maxMessageSize", maxMessageSize);
        maxConnections      = j.value("maxConnections", maxConnections);
        io_thread_count     = j.value("io_thread_count", io_thread_count);
        worker_thread_count = j.value("worker_thread_count", worker_thread_count);
        connection_timeout  = j.value("connection_timeout", connection_timeout);
        read_timeout        = j.value("read_timeout", read_timeout);
        write_timeout       = j.value("write_timeout", write_timeout);
        idle_poll_interval_ms = j.value("idle_poll_interval_ms", idle_poll_interval_ms);
        if (idle_poll_interval_ms < 200) idle_poll_interval_ms = 200;
        if (idle_poll_interval_ms > 60000) idle_poll_interval_ms = 60000;
        max_auth_attempts   = j.value("max_auth_attempts", max_auth_attempts);
        log_level           = j.value("log_level", log_level);
        log_file            = resolve_path(base_dir, j.value("log_file", log_file));
        log_to_console      = j.value("log_to_console", log_to_console);
        log_to_file         = j.value("log_to_file", log_to_file);

        certFile = resolve_path(base_dir, j.value("certFile", certFile));
        keyFile  = resolve_path(base_dir, j.value("keyFile", keyFile));
        dhFile   = resolve_path(base_dir, j.value("dhFile", dhFile));
        enable_tls1_3 = j.value("enable_tls1_3", enable_tls1_3);

        metrics_enabled = j.value("metrics_enabled", metrics_enabled);
        metrics_port = j.value("metrics_port", metrics_port);
        metrics_bind_address = j.value("metrics_bind_address", metrics_bind_address);
        intrusion_detection_enabled = j.value("intrusion_detection_enabled", false);
        intrusion_persist_interval_sec = j.value("intrusion_persist_interval_sec", 60);
        intrusion_persist_dirty_threshold = j.value("intrusion_persist_dirty_threshold", 256);
        intrusion_max_records = j.value("intrusion_max_records", 10000);
        intrusion_ban_threshold = j.value("intrusion_ban_threshold", 0);
        return true;
    }

    virtual bool loadFromFile(const std::string& filename) {
        try {
            std::ifstream input(filename);
            if (!input) return false;
            nlohmann::json json;
            input >> json;
            return loadFromJson(json, std::filesystem::path(filename).parent_path().string());
        } catch (const std::exception&) {
            return false;
        }
    }

    // ---- 校验 (子类应重写以扩展) ----
    virtual bool validate() const {
        if (listeners.empty()) return false;
        bool has_ssl = false;
        for (auto& l : listeners) {
            if (l.port == 0) return false;
            if (l.type == ListenerType::SSL) has_ssl = true;
        }
#ifdef USE_SSL
        if (has_ssl && (certFile.empty() || keyFile.empty())) return false;
#endif
        (void)has_ssl;
        if (io_thread_count == 0 || worker_thread_count == 0) return false;
        if (connection_timeout == 0 || read_timeout == 0 || write_timeout == 0) return false;
        return true;
    }
};

} // namespace pr

#endif // PR_FRAMEWORK_SERVER_CONFIG_BASE_H
