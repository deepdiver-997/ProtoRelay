#ifndef PR_FRAMEWORK_LOG_LOGGER_H
#define PR_FRAMEWORK_LOG_LOGGER_H

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <shared_mutex>
#include <atomic>
#include <iostream>
#include <cstdlib>
#include <filesystem>

namespace pr {

// ================================================================
// Logger —— 字符串注册表式日志核心（2026-10-18 去枚举化）
//
// 以前：模块 = 框架里的 enum + 定长数组 + 应用层 90 个宏三处登记，新增一个
// 业务模块要侵入框架改三处。现在：模块就是一个字符串标签，首次使用惰性
// 注册（复用 init 装好的共享 sink），框架对模块名零知识——新增业务模块
// 直接 LOG_AT("新模块", info, ...)，框架一行不改。
//
// 兼容：应用侧 LOG_<MODULE>_<LEVEL> 便利别名在 mail_system/back/common/logger.h，
// 统一委托 LOG_AT；init/string_to_level/get_instance 签名不变。
// ================================================================
class Logger {
public:
    static Logger& get_instance() {
        static Logger instance;
        return instance;
    }

    void init(const std::string& log_file = "logs/mail_system.log",
              size_t max_file_size = 1024 * 1024 * 5,
              size_t max_files = 3,
              spdlog::level::level_enum level = spdlog::level::info,
              bool enable_console_sink = true,
              bool enable_file_sink = true) {
        std::unique_lock<std::shared_mutex> lock(mu_);
        if (m_initialized) return;

        try {
            if (!enable_console_sink && !enable_file_sink)
                enable_console_sink = true;

            if (enable_console_sink) {
                auto cs = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
                cs->set_level(level);
                cs->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] [%n] %@  %v");
                sinks_.push_back(cs);
            }

            if (enable_file_sink) {
                std::filesystem::path lp(log_file);
                if (!lp.parent_path().empty() && !std::filesystem::exists(lp.parent_path()))
                    std::filesystem::create_directories(lp.parent_path());
                auto fs = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                    log_file, max_file_size, max_files);
                fs->set_level(level);
                fs->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] [%n] %@  %v");
                sinks_.push_back(fs);
            }

            level_ = level;
            // E2E 测试模式（PR_E2E_FLUSH_LOGS=1）强制每条 info 立即 flush，
            // 让 Python 进程能实时读到 server.log。否则 spdlog 默认只在
            // warn/error 触发 flush，30s 等待期内写入会留在 buffer。
            const char* e2e_env = std::getenv("PR_E2E_FLUSH_LOGS");
            flush_level_ = (e2e_env && std::string(e2e_env) == "1")
                ? spdlog::level::info : spdlog::level::warn;

            m_initialized = true;
            lock.unlock();

            // 默认 logger = SERVER 模块（保持旧行为：spdlog::info 静态 API 落 SERVER）
            spdlog::set_default_logger(get("SERVER"));
            spdlog::info("Logger initialized");
        } catch (const spdlog::spdlog_ex& ex) {
            std::cerr << "Log init failed: " << ex.what() << std::endl;
        }
    }

    static spdlog::level::level_enum string_to_level(const std::string& s) {
        if (s == "trace")    return spdlog::level::trace;
        if (s == "debug")    return spdlog::level::debug;
        if (s == "info")     return spdlog::level::info;
        if (s == "warn")     return spdlog::level::warn;
        if (s == "error")    return spdlog::level::err;
        if (s == "critical") return spdlog::level::critical;
        if (s == "off")      return spdlog::level::off;
        return spdlog::level::info;
    }

    // 惰性注册：模块标签首次使用时创建并复用共享 sink；未 init（测试零配置）
    // 返回 spdlog 默认 logger。string_view + 透明比较器：热路径查找零分配。
    std::shared_ptr<spdlog::logger> get(std::string_view module) {
        {
            std::shared_lock<std::shared_mutex> lock(mu_);
            if (!m_initialized) return spdlog::default_logger();
            auto it = loggers_.find(module);
            if (it != loggers_.end()) return it->second;
        }
        std::unique_lock<std::shared_mutex> lock(mu_);
        // double-check：可能已被并发首次使用注册
        auto it = loggers_.find(module);
        if (it != loggers_.end()) return it->second;
        auto lg = std::make_shared<spdlog::logger>(std::string(module),
                                                   sinks_.begin(), sinks_.end());
        lg->set_level(level_);
        lg->flush_on(flush_level_);
        loggers_.emplace(std::string(module), lg);
        return lg;
    }

    void set_level(spdlog::level::level_enum lv) {
        std::unique_lock<std::shared_mutex> lock(mu_);
        level_ = lv;
        for (auto& [name, lg] : loggers_) lg->set_level(lv);
        lock.unlock();
        spdlog::set_level(lv);
    }

    void set_module_level(std::string_view module, spdlog::level::level_enum lv) {
        std::shared_lock<std::shared_mutex> lock(mu_);
        auto it = loggers_.find(module);
        if (it != loggers_.end()) it->second->set_level(lv);
    }

    void flush() {
        std::shared_lock<std::shared_mutex> lock(mu_);
        for (auto& [name, lg] : loggers_) lg->flush();
        lock.unlock();
        spdlog::default_logger()->flush();
    }

    void shutdown() { flush(); spdlog::shutdown(); }

private:
    Logger() = default;
    ~Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    bool m_initialized = false;
    std::vector<spdlog::sink_ptr> sinks_;
    // 透明比较器：string_view 键查找免构造 std::string（热路径零分配）
    std::map<std::string, std::shared_ptr<spdlog::logger>, std::less<>> loggers_;
    spdlog::level::level_enum level_ = spdlog::level::info;
    spdlog::level::level_enum flush_level_ = spdlog::level::warn;
    mutable std::shared_mutex mu_;
};

// ---- 便捷函数 ----
inline std::shared_ptr<spdlog::logger> log(std::string_view module) {
    return Logger::get_instance().get(module);
}
inline void set_log_level(spdlog::level::level_enum lv) {
    Logger::get_instance().set_level(lv);
}
inline void set_module_log_level(std::string_view module, spdlog::level::level_enum lv) {
    Logger::get_instance().set_module_level(module, lv);
}

} // namespace pr

#endif // PR_FRAMEWORK_LOG_LOGGER_H
