#ifndef PR_FRAMEWORK_LOG_LOGGER_H
#define PR_FRAMEWORK_LOG_LOGGER_H

#include "framework/log/log_format.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace pr {

enum class LogLevel { trace, debug, info, warn, err, critical, off };

struct SourceLocation {
    const char* file = "";
    int line = 0;
    const char* function = "";
};

// Owns message/module so a handler can copy the record for asynchronous delivery.
// SourceLocation strings must have static lifetime (as supplied by LOG_AT).
struct LogRecord {
    LogLevel level;
    std::string module;
    std::string message;
    SourceLocation source;
    std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now();
};

class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(const LogRecord& record) = 0;
    virtual void flush() {}
};

using LogHandler = std::function<void(const LogRecord&)>;

class Logger {
public:
    static Logger& get_instance();

    void init(const std::string& log_file = "logs/mail_system.log",
              size_t max_file_size = 1024 * 1024 * 5, size_t max_files = 3,
              LogLevel level = LogLevel::info,
              bool enable_console_sink = true, bool enable_file_sink = true);

    // Explicit injection wins over later init() calls, including bootstrap init.
    // Handlers may run concurrently and must synchronize their own mutable state.
    // No registry lock is held when calling user code. Empty handler = no output.
    void set_sink(std::shared_ptr<LogSink> sink);
    void set_handler(LogHandler handler, std::function<void()> flush = {});
    bool should_log(std::string_view module, LogLevel level) const;
    void write(const LogRecord& record) noexcept;
    void set_level(LogLevel level);
    void set_module_level(std::string_view module, LogLevel level);
    void flush() noexcept;
    void shutdown() noexcept;
    uint64_t errors_total() const { return errors_.load(std::memory_order_relaxed); }
    void record_error() noexcept { errors_.fetch_add(1, std::memory_order_relaxed); }
    static LogLevel string_to_level(const std::string& value);

private:
    Logger();
    mutable std::mutex mutex_;
    std::shared_ptr<LogSink> sink_;
    std::map<std::string, LogLevel, std::less<>> levels_;
    LogLevel level_ = LogLevel::info;
    bool configured_ = false;
    std::atomic<uint64_t> errors_{0};
};

template <typename... Args>
void log(std::string_view module, LogLevel level, SourceLocation source,
         std::string_view format, Args&&... args) noexcept {
    auto& logger = Logger::get_instance();
    try {
        if (logger.should_log(module, level))
            logger.write({level, std::string(module),
                          format_log(format, std::forward<Args>(args)...), source});
    } catch (...) {
        // A formatter or user handler must never unwind into an IO/worker loop.
        logger.record_error();
    }
}

inline void set_log_level(LogLevel level) { Logger::get_instance().set_level(level); }
inline void set_module_log_level(std::string_view module, LogLevel level) {
    Logger::get_instance().set_module_level(module, level);
}

} // namespace pr
#endif
