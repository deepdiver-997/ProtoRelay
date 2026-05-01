#ifndef MAIL_SYSTEM_SERVER_CONFIG_H
#define MAIL_SYSTEM_SERVER_CONFIG_H

#include <thread>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <cstdint>
#include <vector>
#include <nlohmann/json.hpp> // JSON库
#include "mail_system/back/db/db_pool.h"

namespace mail_system {

enum class InboundAckMode : int {
    AFTER_PERSIST = 0,
    AFTER_ENQUEUE = 1,
};

inline const char* inbound_ack_mode_to_string(InboundAckMode mode) {
    switch (mode) {
    case InboundAckMode::AFTER_ENQUEUE:
        return "after_enqueue";
    case InboundAckMode::AFTER_PERSIST:
    default:
        return "after_persist";
    }
}

inline InboundAckMode inbound_ack_mode_from_string(const std::string& raw_mode) {
    if (raw_mode == "after_enqueue") {
        return InboundAckMode::AFTER_ENQUEUE;
    }
    return InboundAckMode::AFTER_PERSIST;
}

// 服务器配置类
struct ServerConfig {
    // 网络配置
    std::string address;              // 监听地址
    uint16_t port;                    // 主监听端口
    
    // SSL配置
    bool enable_ssl;                  // 是否启用SSL监听
    bool use_ssl;                     // 是否使用SSL/TLS（向后兼容）
    bool enable_tcp;                  // 是否启用TCP监听
    bool allow_insecure;              // 是否允许非SSL连接（用于内网通信）
    uint16_t ssl_port;                // SSL监听端口
    uint16_t tcp_port;                // TCP监听端口
    uint16_t insecure_port;           // 非SSL连接端口（向后兼容）
    
    std::string certFile;             // SSL证书文件路径
    std::string keyFile;              // SSL私钥文件路径
    std::string dhFile;               // Diffie-Hellman参数文件路径（可选）

    size_t maxMessageSize;            // 最大消息大小
    size_t maxConnections;            // 最大连接数
    
    // 线程池配置
    size_t io_thread_count;           // IO线程池大小
    size_t worker_thread_count;       // 工作线程池大小
    bool ssl_in_worker;
    
    // 数据库配置
    bool use_database;                // 是否使用数据库
    DBPoolConfig db_pool_config;     // 数据库连接池配置
    
    // 超时配置
    uint32_t connection_timeout;      // 连接超时时间（秒）
    uint32_t read_timeout;           // 读取超时时间（秒）
    uint32_t write_timeout;          // 写入超时时间（秒）
    
    // 安全配置
    bool require_auth;               // 是否要求认证
    size_t max_auth_attempts;        // 最大认证尝试次数
    
    // 日志配置
    std::string log_level;           // 日志级别
    std::string log_file;            // 日志文件路径
    bool log_to_console;             // 是否输出到终端
    bool log_to_file;                // 是否输出到文件
    std::string storage_provider;    // 存储适配器类型：local/distributed
    std::string mail_storage_path;   // 邮件存储路径
    std::string attachment_storage_path; // 附件存储路径
    std::vector<std::string> distributed_storage_roots; // 分布式存储节点根目录
    size_t distributed_storage_replica_count; // 写入副本数
    std::string hdfs_endpoint;       // WebHDFS NameNode HTTP endpoint
    std::string hdfs_base_path;      // WebHDFS base path
    std::string hdfs_user;           // WebHDFS user.name
    uint32_t hdfs_timeout_ms;        // WebHDFS request timeout
    size_t hdfs_replication;         // WebHDFS replication factor for CREATE

    // 系统标识与出站策略配置
    std::string system_name;         // 当前邮件系统名称
    std::string system_domain;       // 当前邮件系统的域名（用于内外部判定）
    std::vector<uint16_t> outbound_ports; // 预留：真实SMTP投递端口优先级
    size_t outbound_max_attempts;    // 预留：每条出站记录最大尝试次数
    uint32_t outbound_poll_busy_sleep_ms;   // 负载繁忙时轮询间隔
    uint32_t outbound_poll_backoff_base_ms; // 空闲轮询回退基准
    uint32_t outbound_poll_backoff_max_ms;  // 空闲轮询回退上限
    uint32_t outbound_poll_backoff_shift_cap; // 指数回退最大移位次数

    // 入站确认与持久化背压配置
    InboundAckMode inbound_ack_mode;   // DATA_END 后是等持久化成功再 ACK，还是入队即 ACK
    uint32_t inbound_persist_wait_timeout_ms; // after_persist 模式下等待持久化完成的超时时间
    size_t persist_max_inflight_mails; // 持久化层总在途邮件数上限，0 表示不启用
    size_t persist_min_available_memory_mb; // 可用内存低于该阈值时拒绝新异步入队，0 表示不启用
    size_t persist_min_db_available_connections; // DB 空闲连接低于该阈值时拒绝入队，0 表示不启用

    // 出站投递身份与 DKIM 配置
    std::string outbound_helo_domain;      // EHLO/HELO 使用的域名
    std::string outbound_mail_from_domain; // 若非空，改写 envelope MAIL FROM 域名
    bool outbound_rewrite_header_from;     // 是否将 From 头改写到 outbound_mail_from_domain
    bool outbound_dkim_enabled;            // 是否启用 DKIM（当前先配置占位）
    std::string outbound_dkim_selector;    // DKIM selector
    std::string outbound_dkim_domain;      // DKIM domain (d=)
    std::string outbound_dkim_private_key_file; // DKIM 私钥文件路径

    // Metrics 配置
    bool metrics_enabled;              // 是否启用 metrics HTTP 端点
    uint16_t metrics_port;             // metrics 监听端口
    std::string metrics_bind_address;  // metrics 绑定地址（默认 127.0.0.1）

    ServerConfig()
        : address("0.0.0.0")
        , port(0)
        , enable_ssl(true)            // 默认启用SSL监听
        , use_ssl(true)               // 向后兼容
        , enable_tcp(false)           // 默认不启用TCP监听
        , allow_insecure(false)
        , ssl_port(0)
        , tcp_port(0)
        , insecure_port(0)
        , maxMessageSize(1024 * 1024)  // 1MB
        , maxConnections(1000)
        , io_thread_count(std::thread::hardware_concurrency())
        , worker_thread_count(std::thread::hardware_concurrency())
        , use_database(false)
        , connection_timeout(300)      // 5分钟
        , read_timeout(60)            // 1分钟
        , write_timeout(60)           // 1分钟
        , require_auth(true)
        , max_auth_attempts(3)
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
        , outbound_mail_from_domain("")
        , outbound_rewrite_header_from(true)
        , outbound_dkim_enabled(false)
        , outbound_dkim_selector("default")
        , outbound_dkim_domain("")
        , outbound_dkim_private_key_file("")
        , metrics_enabled(false)
        , metrics_port(9090)
        , metrics_bind_address("127.0.0.1")
    {}

    ServerConfig(const ServerConfig& other) = default;

    // 显示配置
    void show() const {
        std::cout << "ServerConfig: "
                  << "\naddress = " << address
                  << "\nport = " << port
                  << "\nenable_ssl = " << (enable_ssl ? "true" : "false")
                  << "\nuse_ssl = " << (use_ssl ? "true" : "false")
                  << "\nenable_tcp = " << (enable_tcp ? "true" : "false")
                  << "\nallow_insecure = " << (allow_insecure ? "true" : "false")
                  << "\nssl_port = " << ssl_port
                  << "\ntcp_port = " << tcp_port
                  << "\ninsecure_port = " << insecure_port;
        
#ifdef USE_SSL
        if (enable_ssl) {
            std::cout << "\ncertFile = " << certFile
                      << "\nkeyFile = " << keyFile
                      << "\ndhFile = " << dhFile;
        }
#endif
        
        std::cout << "\nmaxMessageSize = " << maxMessageSize
                  << "\nmaxConnections = " << maxConnections
                  << "\nio_thread_count = " << io_thread_count
                  << "\nworker_thread_count = " << worker_thread_count
                  << "\nuse_database = " << (use_database ? "true" : "false");
        
        if (use_database) {
            db_pool_config.show();
        }
        
        std::cout << "\nconnection_timeout = " << connection_timeout
                  << "\nread_timeout = " << read_timeout
                  << "\nwrite_timeout = " << write_timeout
                  << "\nrequire_auth = " << (require_auth ? "true" : "false")
                  << "\nmax_auth_attempts = " << max_auth_attempts
                  << "\nlog_level = " << log_level
                  << "\nlog_file = " << log_file
                  << "\nlog_to_console = " << (log_to_console ? "true" : "false")
                  << "\nlog_to_file = " << (log_to_file ? "true" : "false")
                  << "\nstorage_provider = " << storage_provider
                  << "\nmail_storage_path = " << mail_storage_path
                  << "\nattachment_storage_path = " << attachment_storage_path
                  << "\ndistributed_storage_replica_count = " << distributed_storage_replica_count
                  << "\nhdfs_endpoint = " << hdfs_endpoint
                  << "\nhdfs_base_path = " << hdfs_base_path
                  << "\nhdfs_user = " << hdfs_user
                  << "\nhdfs_timeout_ms = " << hdfs_timeout_ms
                  << "\nhdfs_replication = " << hdfs_replication
                  << "\nsystem_name = " << system_name
                  << "\nsystem_domain = " << system_domain
                  << "\noutbound_max_attempts = " << outbound_max_attempts
                  << "\noutbound_poll_busy_sleep_ms = " << outbound_poll_busy_sleep_ms
                  << "\noutbound_poll_backoff_base_ms = " << outbound_poll_backoff_base_ms
                  << "\noutbound_poll_backoff_max_ms = " << outbound_poll_backoff_max_ms
                  << "\noutbound_poll_backoff_shift_cap = " << outbound_poll_backoff_shift_cap
                  << "\ninbound_ack_mode = " << inbound_ack_mode_to_string(inbound_ack_mode)
                  << "\ninbound_persist_wait_timeout_ms = " << inbound_persist_wait_timeout_ms
                  << "\npersist_max_inflight_mails = " << persist_max_inflight_mails
                  << "\npersist_min_available_memory_mb = " << persist_min_available_memory_mb
                  << "\npersist_min_db_available_connections = " << persist_min_db_available_connections
                  << "\noutbound_helo_domain = " << outbound_helo_domain
                  << "\noutbound_mail_from_domain = " << outbound_mail_from_domain
                  << "\noutbound_rewrite_header_from = " << (outbound_rewrite_header_from ? "true" : "false")
                  << "\noutbound_dkim_enabled = " << (outbound_dkim_enabled ? "true" : "false")
                  << "\noutbound_dkim_selector = " << outbound_dkim_selector
                  << "\noutbound_dkim_domain = " << outbound_dkim_domain
                  << "\noutbound_dkim_private_key_file = " << outbound_dkim_private_key_file
                  << "\nmetrics_enabled = " << (metrics_enabled ? "true" : "false")
                  << "\nmetrics_port = " << metrics_port
                  << "\nmetrics_bind_address = " << metrics_bind_address
                  << std::endl;

        std::cout << "outbound_ports = [";
        for (size_t i = 0; i < outbound_ports.size(); ++i) {
            std::cout << outbound_ports[i];
            if (i + 1 < outbound_ports.size()) {
                std::cout << ", ";
            }
        }
        std::cout << "]" << std::endl;

        if (!distributed_storage_roots.empty()) {
            std::cout << "distributed_storage_roots = [";
            for (size_t i = 0; i < distributed_storage_roots.size(); ++i) {
                std::cout << distributed_storage_roots[i];
                if (i + 1 < distributed_storage_roots.size()) {
                    std::cout << ", ";
                }
            }
            std::cout << "]" << std::endl;
        }
    }
    
    // 设置向后兼容的端口（非const方法）
    // void setup_backward_compatibility_ports();
    
    // 验证配置是否有效
    bool validate() const {
        // 至少需要启用一种连接类型
        if (!enable_ssl && !enable_tcp) {
            std::cerr << "Error: At least one of enable_ssl or enable_tcp must be true" << std::endl;
            return false;
        }
        
        // 端口验证
        uint16_t actual_ssl_port = (ssl_port != 0) ? ssl_port : port;
        uint16_t actual_tcp_port = (tcp_port != 0) ? tcp_port : port;
        
        if (enable_ssl && actual_ssl_port == 0) {
            std::cerr << "Error: SSL enabled but no port specified" << std::endl;
            return false;
        }
        
        if (enable_tcp && actual_tcp_port == 0) {
            std::cerr << "Error: TCP enabled but no port specified" << std::endl;
            return false;
        }
        
#ifdef USE_SSL
        // SSL配置验证
        if (enable_ssl && (certFile.empty() || keyFile.empty())) {
            std::cerr << "Error: SSL enabled but certificate or key file not specified" << std::endl;
            return false;
        }
#endif
        
        // 线程池配置验证
        if (io_thread_count == 0 || worker_thread_count == 0) {
            std::cerr << "Error: Thread pool sizes must be greater than 0" << std::endl;
            return false;
        }
        
        // 超时配置验证
        if (connection_timeout == 0 || read_timeout == 0 || write_timeout == 0) {
            std::cerr << "Error: Timeout values must be greater than 0" << std::endl;
            return false;
        }
        if (inbound_persist_wait_timeout_ms == 0) {
            std::cerr << "Error: inbound_persist_wait_timeout_ms must be greater than 0" << std::endl;
            return false;
        }

        if (storage_provider == "distributed" && distributed_storage_roots.empty()) {
            std::cerr << "Error: storage_provider=distributed requires distributed_storage_roots" << std::endl;
            return false;
        }

        if (storage_provider == "hdfs_web") {
#if !PROTORELAY_ENABLE_HDFS_WEB_STORAGE
            std::cerr << "Error: storage_provider=hdfs_web requires ENABLE_HDFS_WEB_STORAGE=ON at build time" << std::endl;
            return false;
#endif
            if (hdfs_endpoint.empty() || hdfs_base_path.empty() || hdfs_user.empty()) {
                std::cerr << "Error: storage_provider=hdfs_web requires hdfs_endpoint/hdfs_base_path/hdfs_user" << std::endl;
                return false;
            }
            if (hdfs_timeout_ms == 0 || hdfs_replication == 0) {
                std::cerr << "Error: hdfs_timeout_ms and hdfs_replication must be greater than 0" << std::endl;
                return false;
            }
        }
        
        return true;
    }
    
    // 设置向后兼容的端口（非const方法）
    void setup_backward_compatibility_ports() {
        // 向后兼容：根据use_ssl设置enable_ssl
        if (use_ssl && !enable_ssl) {
            enable_ssl = true;
        }
        
        // 向后兼容：如果设置了主端口且没有设置具体端口，则使用主端口
        if (port != 0) {
            if (enable_ssl && ssl_port == 0) ssl_port = port;
            if (enable_tcp && tcp_port == 0) tcp_port = port;
        }
        
        // 如果允许非SSL连接但没有指定非SSL端口，使用TCP端口
        if (allow_insecure && insecure_port == 0 && enable_tcp) {
            insecure_port = tcp_port;
        }
    }

    std::string resolve_path(const std::string& config_path, const std::string& relative_path) {
        if (relative_path.empty()) {
            return "";
        }
        std::filesystem::path config_dir = std::filesystem::path(config_path).parent_path();
        std::filesystem::path resolved_path = (config_dir / relative_path).lexically_normal();
        return std::filesystem::absolute(resolved_path).string();
    }
    
    // 从配置文件加载配置
    bool loadFromFile(const std::string& filename) {
        // TODO: 实现从配置文件加载配置的功能
        // 可以支持JSON、YAML或其他格式
        //using json type for example
        std::ifstream config_file(filename);
        if (!config_file.is_open()) {
            std::cerr << "Failed to open config file: " << filename << std::endl;
            return false;
        }

        nlohmann::json json_config;
        config_file >> json_config;

        // 解析JSON配置
        address = json_config.value("address", address);
        port = json_config.value("port", port);
        use_ssl = json_config.value("use_ssl", use_ssl);
        enable_ssl = json_config.value("enable_ssl", enable_ssl);
        enable_tcp = json_config.value("enable_tcp", enable_tcp);
        allow_insecure = json_config.value("allow_insecure", allow_insecure);
        ssl_port = json_config.value("ssl_port", ssl_port);
        tcp_port = json_config.value("tcp_port", tcp_port);
        insecure_port = json_config.value("insecure_port", insecure_port);
        
#ifdef USE_SSL
        certFile = resolve_path(filename, json_config.value("certFile", certFile));
        keyFile = resolve_path(filename, json_config.value("keyFile", keyFile));
        dhFile = resolve_path(filename, json_config.value("dhFile", dhFile));
#endif
        
        maxMessageSize = json_config.value("maxMessageSize", maxMessageSize);
        maxConnections = json_config.value("maxConnections", maxConnections);
        io_thread_count = json_config.value("io_thread_count", io_thread_count);
        worker_thread_count = json_config.value("worker_thread_count", worker_thread_count);
        use_database = json_config.value("use_database", use_database);
        if (use_database) {
            std::string db_config_file = resolve_path(filename, json_config.value("db_config_file", ""));
            db_pool_config.loadFromJson(db_config_file);
        }
        connection_timeout = json_config.value("connection_timeout", connection_timeout);
        read_timeout = json_config.value("read_timeout", read_timeout);
        write_timeout = json_config.value("write_timeout", write_timeout);
        require_auth = json_config.value("require_auth", require_auth);
        max_auth_attempts = json_config.value("max_auth_attempts", max_auth_attempts);
        log_level = json_config.value("log_level", log_level);
        log_file = resolve_path(filename, json_config.value("log_file", log_file));
        log_to_console = json_config.value("log_to_console", log_to_console);
        log_to_file = json_config.value("log_to_file", log_to_file);
        storage_provider = json_config.value("storage_provider", storage_provider);
        mail_storage_path = resolve_path(filename, json_config.value("mail_storage_path", mail_storage_path));
        attachment_storage_path = resolve_path(filename, json_config.value("attachment_storage_path", attachment_storage_path));
        distributed_storage_replica_count =
            json_config.value("distributed_storage_replica_count", distributed_storage_replica_count);
        hdfs_endpoint = json_config.value("hdfs_endpoint", hdfs_endpoint);
        hdfs_base_path = json_config.value("hdfs_base_path", hdfs_base_path);
        hdfs_user = json_config.value("hdfs_user", hdfs_user);
        hdfs_timeout_ms = json_config.value("hdfs_timeout_ms", hdfs_timeout_ms);
        hdfs_replication = json_config.value("hdfs_replication", hdfs_replication);

        if (json_config.contains("distributed_storage_roots") &&
            json_config["distributed_storage_roots"].is_array()) {
            distributed_storage_roots.clear();
            for (const auto& item : json_config["distributed_storage_roots"]) {
                if (!item.is_string()) {
                    continue;
                }
                auto resolved = resolve_path(filename, item.get<std::string>());
                if (!resolved.empty()) {
                    distributed_storage_roots.push_back(std::move(resolved));
                }
            }
        }
        system_name = json_config.value("system_name", system_name);
        system_domain = json_config.value("system_domain", system_domain);
        outbound_max_attempts = json_config.value("outbound_max_attempts", outbound_max_attempts);
        outbound_poll_busy_sleep_ms = json_config.value("outbound_poll_busy_sleep_ms", outbound_poll_busy_sleep_ms);
        outbound_poll_backoff_base_ms = json_config.value("outbound_poll_backoff_base_ms", outbound_poll_backoff_base_ms);
        outbound_poll_backoff_max_ms = json_config.value("outbound_poll_backoff_max_ms", outbound_poll_backoff_max_ms);
        outbound_poll_backoff_shift_cap = json_config.value("outbound_poll_backoff_shift_cap", outbound_poll_backoff_shift_cap);
        inbound_ack_mode = inbound_ack_mode_from_string(
            json_config.value("inbound_ack_mode", std::string(inbound_ack_mode_to_string(inbound_ack_mode))));
        inbound_persist_wait_timeout_ms =
            json_config.value("inbound_persist_wait_timeout_ms", inbound_persist_wait_timeout_ms);
        persist_max_inflight_mails =
            json_config.value("persist_max_inflight_mails", persist_max_inflight_mails);
        persist_min_available_memory_mb =
            json_config.value("persist_min_available_memory_mb", persist_min_available_memory_mb);
        persist_min_db_available_connections =
            json_config.value("persist_min_db_available_connections", persist_min_db_available_connections);
        outbound_helo_domain = json_config.value("outbound_helo_domain", outbound_helo_domain);
        outbound_mail_from_domain = json_config.value("outbound_mail_from_domain", outbound_mail_from_domain);
        outbound_rewrite_header_from = json_config.value("outbound_rewrite_header_from", outbound_rewrite_header_from);
        outbound_dkim_enabled = json_config.value("outbound_dkim_enabled", outbound_dkim_enabled);
        outbound_dkim_selector = json_config.value("outbound_dkim_selector", outbound_dkim_selector);
        outbound_dkim_domain = json_config.value("outbound_dkim_domain", outbound_dkim_domain);
        outbound_dkim_private_key_file = resolve_path(filename,
            json_config.value("outbound_dkim_private_key_file", outbound_dkim_private_key_file));

        metrics_enabled = json_config.value("metrics_enabled", metrics_enabled);
        metrics_port = json_config.value("metrics_port", metrics_port);
        metrics_bind_address = json_config.value("metrics_bind_address", metrics_bind_address);

        if (json_config.contains("outbound_ports") && json_config["outbound_ports"].is_array()) {
            std::vector<uint16_t> parsed_ports;
            for (const auto& item : json_config["outbound_ports"]) {
                if (!item.is_number_unsigned()) {
                    continue;
                }
                const auto parsed = static_cast<uint32_t>(item.get<uint32_t>());
                if (parsed > 0 && parsed <= 65535) {
                    parsed_ports.push_back(static_cast<uint16_t>(parsed));
                }
            }
            if (!parsed_ports.empty()) {
                outbound_ports = std::move(parsed_ports);
            }
        }

        // 设置向后兼容的端口
        setup_backward_compatibility_ports();

        return true;
    }
    
    // 保存配置到文件
    bool saveToFile(const std::string& filename) const {
        // TODO: 实现保存配置到文件的功能
        return true;
    }
};

} // namespace mail_system

#endif // MAIL_SYSTEM_SERVER_CONFIG_H
