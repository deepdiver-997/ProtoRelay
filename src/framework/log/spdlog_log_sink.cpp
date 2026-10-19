#include "framework/log/log_backends.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <cstdlib>
#include <filesystem>
#include <vector>

namespace pr {
namespace {
class SpdlogLogSink final : public LogSink {
public:
    explicit SpdlogLogSink(const LogConfig& config) {
        if (config.console) {
            auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] [%n] %@  %v");
            sinks_.push_back(std::move(sink));
        }
        if (config.file_output) {
            auto path = std::filesystem::path(config.file);
            if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
            spdlog::sink_ptr sink;
            if (config.max_file_size)
                sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(config.file, config.max_file_size, config.max_files);
            else
                sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(config.file);
            sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] [%n] %@  %v");
            sinks_.push_back(std::move(sink));
        }
        const char* value = std::getenv("PR_E2E_FLUSH_LOGS");
        flush_level_ = value && std::string_view(value) == "1" ? spdlog::level::info : spdlog::level::warn;
    }
    void write(const LogRecord& record) override {
        auto logger = get(record.module);
        logger->log(record.timestamp,
                    spdlog::source_loc{record.source.file, record.source.line, record.source.function},
                    static_cast<spdlog::level::level_enum>(record.level), record.message);
    }
    void flush() override {
        for (const auto& sink : sinks_) sink->flush();
    }
private:
    std::shared_ptr<spdlog::logger> get(const std::string& module) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& logger = loggers_[module];
        if (!logger) {
            logger = std::make_shared<spdlog::logger>(module, sinks_.begin(), sinks_.end());
            logger->set_level(spdlog::level::trace);
            logger->flush_on(flush_level_);
        }
        return logger;
    }
    std::vector<spdlog::sink_ptr> sinks_;
    std::map<std::string, std::shared_ptr<spdlog::logger>, std::less<>> loggers_;
    spdlog::level::level_enum flush_level_;
    std::mutex mutex_;
};
}
std::shared_ptr<LogSink> make_spdlog_log_sink(const LogConfig& config) {
    return std::make_shared<SpdlogLogSink>(config);
}
} // namespace pr
