#include "framework/log/log_backends.h"
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <thread>

namespace pr {
namespace {
class StandardLogSink final : public LogSink {
public:
    explicit StandardLogSink(LogConfig config) : config_(std::move(config)) {
        if (config_.file_output) {
            auto path = std::filesystem::path(config_.file);
            if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
            open_file();
        }
        const char* value = std::getenv("PR_E2E_FLUSH_LOGS");
        flush_info_ = value && std::string_view(value) == "1";
    }
    void write(const LogRecord& record) override {
        static constexpr const char* names[] = {"trace", "debug", "info", "warning", "error", "critical", "off"};
        const auto seconds = std::chrono::system_clock::to_time_t(record.timestamp);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &seconds);
#else
        localtime_r(&seconds, &local);
#endif
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(record.timestamp.time_since_epoch()).count() % 1000;
        std::ostringstream out;
        out << '[' << std::put_time(&local, "%Y-%m-%d %H:%M:%S") << '.'
            << std::setfill('0') << std::setw(3) << ms << "] ["
            << names[static_cast<size_t>(record.level)] << "] [" << std::this_thread::get_id()
            << "] [" << record.module << "] ";
        if (record.source.file && *record.source.file)
            out << std::filesystem::path(record.source.file).filename().string() << ':' << record.source.line << "  ";
        out << record.message << '\n';
        const auto line = out.str();
        std::lock_guard<std::mutex> lock(mutex_);
        if (config_.console) std::cout << line;
        if (config_.file_output) {
            if (config_.max_file_size && bytes_ && bytes_ + line.size() > config_.max_file_size) rotate();
            file_ << line;
            bytes_ += line.size();
        }
        if (flush_info_ || record.level >= LogLevel::warn) flush_locked();
    }
    void flush() override { std::lock_guard<std::mutex> lock(mutex_); flush_locked(); }
private:
    void open_file() {
        file_.open(config_.file, std::ios::out | std::ios::app | std::ios::binary);
        if (!file_) throw std::runtime_error("Cannot open log file: " + config_.file);
        bytes_ = static_cast<size_t>(file_.tellp());
    }
    std::filesystem::path rotated(size_t index) const {
        const std::filesystem::path path(config_.file);
        return path.parent_path() / (path.stem().string() + "." + std::to_string(index) + path.extension().string());
    }
    void rotate() {
        file_.close();
        if (config_.max_files) {
            std::filesystem::remove(rotated(config_.max_files));
            for (size_t i = config_.max_files; i > 1; --i)
                if (std::filesystem::exists(rotated(i - 1))) std::filesystem::rename(rotated(i - 1), rotated(i));
            std::filesystem::rename(config_.file, rotated(1));
        } else {
            std::filesystem::remove(config_.file);
        }
        open_file();
    }
    void flush_locked() { if (config_.console) std::cout.flush(); if (file_.is_open()) file_.flush(); }
    LogConfig config_;
    std::ofstream file_;
    size_t bytes_ = 0;
    bool flush_info_ = false;
    std::mutex mutex_;
};
}
std::shared_ptr<LogSink> make_standard_log_sink(const LogConfig& config) {
    return std::make_shared<StandardLogSink>(config);
}
} // namespace pr
