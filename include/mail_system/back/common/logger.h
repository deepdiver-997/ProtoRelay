#ifndef MAIL_SYSTEM_LOGGER_H
#define MAIL_SYSTEM_LOGGER_H

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <memory>
#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <filesystem>

namespace mail_system {

// 日志模块定义
enum class LogModule {
    SERVER,        // 服务器基础日志
    NETWORK,       // 网络连接日志
    DATABASE,      // 数据库连接池日志
    DATABASE_QUERY, // 数据库查询详细日志
    SMTP,          // SMTP 协议日志
    SMTP_DETAIL,   // SMTP 详细状态机日志
    SESSION,       // 会话管理日志
    PERSISTENT_QUEUE, // 持久化队列日志
    OUTBOUND,      // 出站投递客户端/仓储日志
    THREAD_POOL,   // 线程池日志
    FILE_IO,       // 文件 I/O 日志
    AUTH           // 认证日志
};

// 日志级别转换
inline spdlog::level::level_enum to_spdlog_level(spdlog::level::level_enum level) {
    return level;
}

// 日志系统管理类
class Logger {
public:
    static Logger& get_instance() {
        static Logger instance;
        return instance;
    }

    // 初始化日志系统
    void init(const std::string& log_file = "logs/mail_system.log",
              size_t max_file_size = 1024 * 1024 * 5,  // 5MB
              size_t max_files = 3,
              spdlog::level::level_enum level = spdlog::level::info,
              bool enable_console_sink = true,
              bool enable_file_sink = true) {
        if (m_initialized) {
            return;
        }

        try {
            // 避免配置错误导致完全无日志
            if (!enable_console_sink && !enable_file_sink) {
                enable_console_sink = true;
            }

            std::vector<spdlog::sink_ptr> sinks;

            if (enable_console_sink) {
                auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
                console_sink->set_level(level);
                console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] [%n] %v");
                sinks.push_back(console_sink);
            }

            if (enable_file_sink) {
                // 确保日志目录存在
                std::filesystem::path log_dir(log_file);
                if (!log_dir.parent_path().empty() && !std::filesystem::exists(log_dir.parent_path())) {
                    std::filesystem::create_directories(log_dir.parent_path());
                }

                auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                    log_file, max_file_size, max_files);
                file_sink->set_level(level);
                file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] [%n] %v");
                sinks.push_back(file_sink);
            }

            // 创建各模块 logger
            m_loggers[static_cast<size_t>(LogModule::SERVER)] =
                std::make_shared<spdlog::logger>("SERVER", sinks.begin(), sinks.end());
            m_loggers[static_cast<size_t>(LogModule::NETWORK)] =
                std::make_shared<spdlog::logger>("NETWORK", sinks.begin(), sinks.end());
            m_loggers[static_cast<size_t>(LogModule::DATABASE)] =
                std::make_shared<spdlog::logger>("DATABASE", sinks.begin(), sinks.end());
            m_loggers[static_cast<size_t>(LogModule::DATABASE_QUERY)] =
                std::make_shared<spdlog::logger>("DB_QUERY", sinks.begin(), sinks.end());
            m_loggers[static_cast<size_t>(LogModule::SMTP)] =
                std::make_shared<spdlog::logger>("SMTP", sinks.begin(), sinks.end());
            m_loggers[static_cast<size_t>(LogModule::SMTP_DETAIL)] =
                std::make_shared<spdlog::logger>("SMTP_DETAIL", sinks.begin(), sinks.end());
            m_loggers[static_cast<size_t>(LogModule::SESSION)] =
                std::make_shared<spdlog::logger>("SESSION", sinks.begin(), sinks.end());
            m_loggers[static_cast<size_t>(LogModule::PERSISTENT_QUEUE)] =
                std::make_shared<spdlog::logger>("PERSISTENT_QUEUE", sinks.begin(), sinks.end());
            m_loggers[static_cast<size_t>(LogModule::OUTBOUND)] =
                std::make_shared<spdlog::logger>("OUTBOUND", sinks.begin(), sinks.end());
            m_loggers[static_cast<size_t>(LogModule::THREAD_POOL)] =
                std::make_shared<spdlog::logger>("THREAD_POOL", sinks.begin(), sinks.end());
            m_loggers[static_cast<size_t>(LogModule::FILE_IO)] =
                std::make_shared<spdlog::logger>("FILE_IO", sinks.begin(), sinks.end());
            m_loggers[static_cast<size_t>(LogModule::AUTH)] =
                std::make_shared<spdlog::logger>("AUTH", sinks.begin(), sinks.end());

            // 设置默认 logger
            spdlog::set_default_logger(m_loggers[static_cast<size_t>(LogModule::SERVER)]);
            spdlog::set_level(level);
            spdlog::flush_on(spdlog::level::warn);

            m_initialized = true;
            spdlog::info("Logger initialized");
        } catch (const spdlog::spdlog_ex& ex) {
            std::cerr << "Log init failed: " << ex.what() << std::endl;
        }
    }

    static spdlog::level::level_enum string_to_level(const std::string& level_str) {
        if (level_str == "trace") return spdlog::level::trace;
        if (level_str == "debug") return spdlog::level::debug;
        if (level_str == "info") return spdlog::level::info;
        if (level_str == "warn") return spdlog::level::warn;
        if (level_str == "error") return spdlog::level::err;
        if (level_str == "critical") return spdlog::level::critical;
        if (level_str == "off") return spdlog::level::off;
        return spdlog::level::info; // 默认级别
    }

    // 获取指定模块的 logger
    std::shared_ptr<spdlog::logger> get_logger(LogModule module) {
        size_t index = static_cast<size_t>(module);
        if (index < m_loggers.size() && m_loggers[index]) {
            return m_loggers[index];
        }
        return spdlog::default_logger();
    }

    // 设置全局日志级别
    void set_level(spdlog::level::level_enum level) {
        spdlog::set_level(level);
        for (auto& logger : m_loggers) {
            if (logger) {
                logger->set_level(level);
            }
        }
    }

    // 设置指定模块的日志级别
    void set_module_level(LogModule module, spdlog::level::level_enum level) {
        size_t index = static_cast<size_t>(module);
        if (index < m_loggers.size() && m_loggers[index]) {
            m_loggers[index]->set_level(level);
        }
    }

    // 冲刷日志
    void flush() {
        for (auto& logger : m_loggers) {
            if (logger) {
                logger->flush();
            }
        }
        spdlog::default_logger()->flush();
    }

    // 关闭日志系统
    void shutdown() {
        flush();
        spdlog::shutdown();
    }

private:
    Logger() = default;
    ~Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    bool m_initialized = false;
    std::array<std::shared_ptr<spdlog::logger>, 12> m_loggers;
};

// 便捷函数：获取 logger
inline std::shared_ptr<spdlog::logger> log(LogModule module) {
    return Logger::get_instance().get_logger(module);
}

// 设置日志级别的便捷函数
inline void set_log_level(spdlog::level::level_enum level) {
    Logger::get_instance().set_level(level);
}

inline void set_module_log_level(LogModule module, spdlog::level::level_enum level) {
    Logger::get_instance().set_module_level(module, level);
}

} // namespace mail_system

// ==================== 模块化日志宏控制 ====================

// 定义各模块是否启用调试日志的宏（默认都关闭，需要时开启）
#ifndef ENABLE_SERVER_DEBUG_LOG
#define ENABLE_SERVER_DEBUG_LOG 0
#endif
#ifndef ENABLE_NETWORK_DEBUG_LOG
#define ENABLE_NETWORK_DEBUG_LOG 0
#endif
#ifndef ENABLE_DATABASE_DEBUG_LOG
#define ENABLE_DATABASE_DEBUG_LOG 0
#endif
#ifndef ENABLE_DATABASE_QUERY_DEBUG_LOG
#define ENABLE_DATABASE_QUERY_DEBUG_LOG 0    // 数据库查询详细日志（通常关闭）
#endif
#ifndef ENABLE_SMTP_DEBUG_LOG
#define ENABLE_SMTP_DEBUG_LOG 0
#endif
#ifndef ENABLE_SMTP_DETAIL_DEBUG_LOG
#define ENABLE_SMTP_DETAIL_DEBUG_LOG 0      // SMTP 状态机详细日志（通常关闭）
#endif
#ifndef ENABLE_SESSION_DEBUG_LOG
#define ENABLE_SESSION_DEBUG_LOG 0
#endif
#ifndef ENABLE_THREAD_POOL_DEBUG_LOG
#define ENABLE_THREAD_POOL_DEBUG_LOG 0
#endif
#ifndef ENABLE_FILE_IO_DEBUG_LOG
#define ENABLE_FILE_IO_DEBUG_LOG 0
#endif
#ifndef ENABLE_AUTH_DEBUG_LOG
#define ENABLE_AUTH_DEBUG_LOG 0
#endif
#ifndef ENABLE_PERSISTENT_QUEUE_DEBUG_LOG
#define ENABLE_PERSISTENT_QUEUE_DEBUG_LOG 0
#endif
#ifndef ENABLE_OUTBOUND_DEBUG_LOG
#define ENABLE_OUTBOUND_DEBUG_LOG 0
#endif

// ==================== 模块化日志宏定义 ====================

// SERVER 模块日志
#define LOG_SERVER_TRACE(...) \
    if constexpr (ENABLE_SERVER_DEBUG_LOG) { \
        mail_system::log(mail_system::LogModule::SERVER)->trace(__VA_ARGS__); \
    }
#define LOG_SERVER_DEBUG(...) \
    if constexpr (ENABLE_SERVER_DEBUG_LOG) { \
        mail_system::log(mail_system::LogModule::SERVER)->debug(__VA_ARGS__); \
    }
#define LOG_SERVER_INFO(...) \
    mail_system::log(mail_system::LogModule::SERVER)->info(__VA_ARGS__)
#define LOG_SERVER_WARN(...) \
    mail_system::log(mail_system::LogModule::SERVER)->warn(__VA_ARGS__)
#define LOG_SERVER_ERROR(...) \
    mail_system::log(mail_system::LogModule::SERVER)->error(__VA_ARGS__)
#define LOG_SERVER_CRITICAL(...) \
    mail_system::log(mail_system::LogModule::SERVER)->critical(__VA_ARGS__)

// NETWORK 模块日志
#define LOG_NETWORK_TRACE(...) \
    if constexpr (ENABLE_NETWORK_DEBUG_LOG) { \
        mail_system::log(mail_system::LogModule::NETWORK)->trace(__VA_ARGS__); \
    }
#define LOG_NETWORK_DEBUG(...) \
    if constexpr (ENABLE_NETWORK_DEBUG_LOG) { \
        mail_system::log(mail_system::LogModule::NETWORK)->debug(__VA_ARGS__); \
    }
#define LOG_NETWORK_INFO(...) \
    mail_system::log(mail_system::LogModule::NETWORK)->info(__VA_ARGS__)
#define LOG_NETWORK_WARN(...) \
    mail_system::log(mail_system::LogModule::NETWORK)->warn(__VA_ARGS__)
#define LOG_NETWORK_ERROR(...) \
    mail_system::log(mail_system::LogModule::NETWORK)->error(__VA_ARGS__)
#define LOG_NETWORK_CRITICAL(...) \
    mail_system::log(mail_system::LogModule::NETWORK)->critical(__VA_ARGS__)

// DATABASE 模块日志（连接池级别）
#define LOG_DATABASE_TRACE(...) \
    if constexpr (ENABLE_DATABASE_DEBUG_LOG) { \
        mail_system::log(mail_system::LogModule::DATABASE)->trace(__VA_ARGS__); \
    }
#define LOG_DATABASE_DEBUG(...) \
    if constexpr (ENABLE_DATABASE_DEBUG_LOG) { \
        mail_system::log(mail_system::LogModule::DATABASE)->debug(__VA_ARGS__); \
    }
#define LOG_DATABASE_INFO(...) \
    mail_system::log(mail_system::LogModule::DATABASE)->info(__VA_ARGS__)
#define LOG_DATABASE_WARN(...) \
    mail_system::log(mail_system::LogModule::DATABASE)->warn(__VA_ARGS__)
#define LOG_DATABASE_ERROR(...) \
    mail_system::log(mail_system::LogModule::DATABASE)->error(__VA_ARGS__)
#define LOG_DATABASE_CRITICAL(...) \
    mail_system::log(mail_system::LogModule::DATABASE)->critical(__VA_ARGS__)

// DATABASE_QUERY 模块日志（查询详细日志，通常关闭）
#define LOG_DB_QUERY_TRACE(...) \
    if constexpr (ENABLE_DATABASE_QUERY_DEBUG_LOG) { \
        mail_system::log(mail_system::LogModule::DATABASE_QUERY)->trace(__VA_ARGS__); \
    }
#define LOG_DB_QUERY_DEBUG(...) \
    if constexpr (ENABLE_DATABASE_QUERY_DEBUG_LOG) { \
        mail_system::log(mail_system::LogModule::DATABASE_QUERY)->debug(__VA_ARGS__); \
    }
#define LOG_DB_QUERY_INFO(...) \
    if constexpr (ENABLE_DATABASE_QUERY_DEBUG_LOG) { \
        mail_system::log(mail_system::LogModule::DATABASE_QUERY)->info(__VA_ARGS__); \
    }
#define LOG_DB_QUERY_WARN(...) \
    if constexpr (ENABLE_DATABASE_QUERY_DEBUG_LOG) { \
        mail_system::log(mail_system::LogModule::DATABASE_QUERY)->warn(__VA_ARGS__); \
    }
#define LOG_DB_QUERY_ERROR(...) \
    mail_system::log(mail_system::LogModule::DATABASE_QUERY)->error(__VA_ARGS__)
#define LOG_DB_QUERY_CRITICAL(...) \
    mail_system::log(mail_system::LogModule::DATABASE_QUERY)->critical(__VA_ARGS__)

// SMTP 模块日志
#define LOG_SMTP_TRACE(...) \
    if constexpr (ENABLE_SMTP_DEBUG_LOG) { \
        mail_system::log(mail_system::LogModule::SMTP)->trace(__VA_ARGS__); \
    }
#define LOG_SMTP_DEBUG(...) \
    if constexpr (ENABLE_SMTP_DEBUG_LOG) { \
        mail_system::log(mail_system::LogModule::SMTP)->debug(__VA_ARGS__); \
    }
#define LOG_SMTP_INFO(...) \
    mail_system::log(mail_system::LogModule::SMTP)->info(__VA_ARGS__)
#define LOG_SMTP_WARN(...) \
    mail_system::log(mail_system::LogModule::SMTP)->warn(__VA_ARGS__)
#define LOG_SMTP_ERROR(...) \
    mail_system::log(mail_system::LogModule::SMTP)->error(__VA_ARGS__)
#define LOG_SMTP_CRITICAL(...) \
    mail_system::log(mail_system::LogModule::SMTP)->critical(__VA_ARGS__)

// SMTP_DETAIL 模块日志（状态机详细日志，通常关闭）
#define LOG_SMTP_DETAIL_TRACE(...) \
    if constexpr (ENABLE_SMTP_DETAIL_DEBUG_LOG) { \
        mail_system::log(mail_system::LogModule::SMTP_DETAIL)->trace(__VA_ARGS__); \
    }
#define LOG_SMTP_DETAIL_DEBUG(...) \
    if constexpr (ENABLE_SMTP_DETAIL_DEBUG_LOG) { \
        mail_system::log(mail_system::LogModule::SMTP_DETAIL)->debug(__VA_ARGS__); \
    }
#define LOG_SMTP_DETAIL_INFO(...) \
    if constexpr (ENABLE_SMTP_DETAIL_DEBUG_LOG) { \
        mail_system::log(mail_system::LogModule::SMTP_DETAIL)->debug(__VA_ARGS__); \
    }
#define LOG_SMTP_DETAIL_WARN(...) \
    if constexpr (ENABLE_SMTP_DETAIL_DEBUG_LOG) { \
        mail_system::log(mail_system::LogModule::SMTP_DETAIL)->warn(__VA_ARGS__); \
    }
#define LOG_SMTP_DETAIL_ERROR(...) \
    mail_system::log(mail_system::LogModule::SMTP_DETAIL)->error(__VA_ARGS__)
#define LOG_SMTP_DETAIL_CRITICAL(...) \
    mail_system::log(mail_system::LogModule::SMTP_DETAIL)->critical(__VA_ARGS__)

// SESSION 模块日志
#define LOG_SESSION_TRACE(...) \
    if constexpr (ENABLE_SESSION_DEBUG_LOG) { \
        mail_system::log(mail_system::LogModule::SESSION)->trace(__VA_ARGS__); \
    }
#define LOG_SESSION_DEBUG(...) \
    if constexpr (ENABLE_SESSION_DEBUG_LOG) { \
        mail_system::log(mail_system::LogModule::SESSION)->debug(__VA_ARGS__); \
    }
#define LOG_SESSION_INFO(...) \
    mail_system::log(mail_system::LogModule::SESSION)->debug(__VA_ARGS__)
#define LOG_SESSION_WARN(...) \
    mail_system::log(mail_system::LogModule::SESSION)->warn(__VA_ARGS__)
#define LOG_SESSION_ERROR(...) \
    mail_system::log(mail_system::LogModule::SESSION)->error(__VA_ARGS__)
#define LOG_SESSION_CRITICAL(...) \
    mail_system::log(mail_system::LogModule::SESSION)->critical(__VA_ARGS__)

// PERSISTENT_QUEUE 模块日志
#define LOG_PERSISTENT_QUEUE_TRACE(...) \
    if constexpr (ENABLE_PERSISTENT_QUEUE_DEBUG_LOG) { \
        mail_system::log(mail_system::LogModule::PERSISTENT_QUEUE)->trace(__VA_ARGS__); \
    }
#define LOG_PERSISTENT_QUEUE_DEBUG(...) \
    if constexpr (ENABLE_PERSISTENT_QUEUE_DEBUG_LOG) { \
        mail_system::log(mail_system::LogModule::PERSISTENT_QUEUE)->debug(__VA_ARGS__); \
    }
#define LOG_PERSISTENT_QUEUE_INFO(...) \
    mail_system::log(mail_system::LogModule::PERSISTENT_QUEUE)->info(__VA_ARGS__)
#define LOG_PERSISTENT_QUEUE_WARN(...) \
    mail_system::log(mail_system::LogModule::PERSISTENT_QUEUE)->warn(__VA_ARGS__)
#define LOG_PERSISTENT_QUEUE_ERROR(...) \
    mail_system::log(mail_system::LogModule::PERSISTENT_QUEUE)->error(__VA_ARGS__)
#define LOG_PERSISTENT_QUEUE_CRITICAL(...) \
    mail_system::log(mail_system::LogModule::PERSISTENT_QUEUE)->critical(__VA_ARGS__)

// THREAD_POOL 模块日志
#define LOG_OUTBOUND_TRACE(...) \
    if constexpr (ENABLE_OUTBOUND_DEBUG_LOG) { \
        mail_system::log(mail_system::LogModule::OUTBOUND)->trace(__VA_ARGS__); \
    }
#define LOG_OUTBOUND_DEBUG(...) \
    if constexpr (ENABLE_OUTBOUND_DEBUG_LOG) { \
        mail_system::log(mail_system::LogModule::OUTBOUND)->debug(__VA_ARGS__); \
    }
#define LOG_OUTBOUND_INFO(...) \
    mail_system::log(mail_system::LogModule::OUTBOUND)->info(__VA_ARGS__)
#define LOG_OUTBOUND_WARN(...) \
    mail_system::log(mail_system::LogModule::OUTBOUND)->warn(__VA_ARGS__)
#define LOG_OUTBOUND_ERROR(...) \
    mail_system::log(mail_system::LogModule::OUTBOUND)->error(__VA_ARGS__)
#define LOG_OUTBOUND_CRITICAL(...) \
    mail_system::log(mail_system::LogModule::OUTBOUND)->critical(__VA_ARGS__)

// THREAD_POOL 模块日志
#define LOG_THREAD_POOL_TRACE(...) \
    if constexpr (ENABLE_THREAD_POOL_DEBUG_LOG) { \
        mail_system::log(mail_system::LogModule::THREAD_POOL)->trace(__VA_ARGS__); \
    }
#define LOG_THREAD_POOL_DEBUG(...) \
    if constexpr (ENABLE_THREAD_POOL_DEBUG_LOG) { \
        mail_system::log(mail_system::LogModule::THREAD_POOL)->debug(__VA_ARGS__); \
    }
#define LOG_THREAD_POOL_INFO(...) \
    mail_system::log(mail_system::LogModule::THREAD_POOL)->info(__VA_ARGS__)
#define LOG_THREAD_POOL_WARN(...) \
    mail_system::log(mail_system::LogModule::THREAD_POOL)->warn(__VA_ARGS__)
#define LOG_THREAD_POOL_ERROR(...) \
    mail_system::log(mail_system::LogModule::THREAD_POOL)->error(__VA_ARGS__)
#define LOG_THREAD_POOL_CRITICAL(...) \
    mail_system::log(mail_system::LogModule::THREAD_POOL)->critical(__VA_ARGS__)

// FILE_IO 模块日志
#define LOG_FILE_IO_TRACE(...) \
    if constexpr (ENABLE_FILE_IO_DEBUG_LOG) { \
        mail_system::log(mail_system::LogModule::FILE_IO)->trace(__VA_ARGS__); \
    }
#define LOG_FILE_IO_DEBUG(...) \
    if constexpr (ENABLE_FILE_IO_DEBUG_LOG) { \
        mail_system::log(mail_system::LogModule::FILE_IO)->debug(__VA_ARGS__); \
    }
#define LOG_FILE_IO_INFO(...) \
    mail_system::log(mail_system::LogModule::FILE_IO)->info(__VA_ARGS__)
#define LOG_FILE_IO_WARN(...) \
    mail_system::log(mail_system::LogModule::FILE_IO)->warn(__VA_ARGS__)
#define LOG_FILE_IO_ERROR(...) \
    mail_system::log(mail_system::LogModule::FILE_IO)->error(__VA_ARGS__)
#define LOG_FILE_IO_CRITICAL(...) \
    mail_system::log(mail_system::LogModule::FILE_IO)->critical(__VA_ARGS__)

// AUTH 模块日志
#define LOG_AUTH_TRACE(...) \
    if constexpr (ENABLE_AUTH_DEBUG_LOG) { \
        mail_system::log(mail_system::LogModule::AUTH)->trace(__VA_ARGS__); \
    }
#define LOG_AUTH_DEBUG(...) \
    if constexpr (ENABLE_AUTH_DEBUG_LOG) { \
        mail_system::log(mail_system::LogModule::AUTH)->debug(__VA_ARGS__); \
    }
#define LOG_AUTH_INFO(...) \
    mail_system::log(mail_system::LogModule::AUTH)->info(__VA_ARGS__)
#define LOG_AUTH_WARN(...) \
    mail_system::log(mail_system::LogModule::AUTH)->warn(__VA_ARGS__)
#define LOG_AUTH_ERROR(...) \
    mail_system::log(mail_system::LogModule::AUTH)->error(__VA_ARGS__)
#define LOG_AUTH_CRITICAL(...) \
    mail_system::log(mail_system::LogModule::AUTH)->critical(__VA_ARGS__)

#endif // MAIL_SYSTEM_LOGGER_H
