#include "framework/log/log_backends.h"
#include <utility>

namespace pr {
namespace {
class CallbackSink final : public LogSink {
public:
    CallbackSink(LogHandler handler, std::function<void()> flush)
        : handler_(std::move(handler)), flush_(std::move(flush)) {}
    void write(const LogRecord& record) override { handler_(record); }
    void flush() override { if (flush_) flush_(); }
private:
    LogHandler handler_;
    std::function<void()> flush_;
};
}

Logger::Logger() {
#if !PROTORELAY_LOG_NONE
    LogConfig config;
    config.file_output = false;
    sink_ = make_standard_log_sink(config);
#endif
}

Logger& Logger::get_instance() {
    static Logger logger;
    return logger;
}

void Logger::init(const std::string& file, size_t max_size, size_t max_files,
                  LogLevel level, bool console, bool file_output) {
    // Build sinks outside the registry lock. Recheck before publishing so an
    // injected callback always wins, even if injection races initialization.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (configured_) return;
    }
    LogConfig config{file, max_size, max_files, console, file_output};
    std::shared_ptr<LogSink> sink;
#if PROTORELAY_HAS_SPDLOG
    sink = make_spdlog_log_sink(config);
#elif !PROTORELAY_LOG_NONE
    sink = make_standard_log_sink(config);
#endif
    std::shared_ptr<LogSink> previous;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (configured_) return;
        previous = std::move(sink_);
        sink_ = std::move(sink);
        level_ = level;
        configured_ = true;
    }
}

void Logger::set_sink(std::shared_ptr<LogSink> sink) {
    std::shared_ptr<LogSink> previous;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        previous = std::move(sink_);
        sink_ = std::move(sink);
        configured_ = true;
    }
    // User-owned sink destructors also run outside the registry lock.
}

void Logger::set_handler(LogHandler handler, std::function<void()> flush) {
    set_sink(handler ? std::make_shared<CallbackSink>(std::move(handler), std::move(flush)) : nullptr);
}

bool Logger::should_log(std::string_view module, LogLevel level) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = levels_.find(module);
    const auto minimum = it == levels_.end() ? level_ : it->second;
    return sink_ && level != LogLevel::off && minimum != LogLevel::off && level >= minimum;
}

void Logger::write(const LogRecord& record) noexcept {
    try {
        std::shared_ptr<LogSink> sink;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = levels_.find(record.module);
            const auto minimum = it == levels_.end() ? level_ : it->second;
            if (record.level == LogLevel::off || minimum == LogLevel::off || record.level < minimum) return;
            sink = sink_;
        }
        if (sink) sink->write(record);
    } catch (...) { record_error(); }
}

void Logger::set_level(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    level_ = level;
}

void Logger::set_module_level(std::string_view module, LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    levels_[std::string(module)] = level;
}

void Logger::flush() noexcept {
    try {
        std::shared_ptr<LogSink> sink;
        { std::lock_guard<std::mutex> lock(mutex_); sink = sink_; }
        if (sink) sink->flush();
    } catch (...) { record_error(); }
}

void Logger::shutdown() noexcept {
    try {
        std::shared_ptr<LogSink> sink;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sink = std::move(sink_);
            configured_ = true;
        }
        if (sink) sink->flush();
    } catch (...) { record_error(); }
}

LogLevel Logger::string_to_level(const std::string& value) {
    if (value == "trace") return LogLevel::trace;
    if (value == "debug") return LogLevel::debug;
    if (value == "warn" || value == "warning") return LogLevel::warn;
    if (value == "error" || value == "err") return LogLevel::err;
    if (value == "critical") return LogLevel::critical;
    if (value == "off") return LogLevel::off;
    return LogLevel::info;
}
} // namespace pr
