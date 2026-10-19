#ifndef PR_FRAMEWORK_LOG_BACKENDS_H
#define PR_FRAMEWORK_LOG_BACKENDS_H
#include "framework/log/logger.h"

namespace pr {
struct LogConfig {
    std::string file = "logs/mail_system.log";
    size_t max_file_size = 5 * 1024 * 1024;
    size_t max_files = 3;
    bool console = true;
    bool file_output = true;
};

std::shared_ptr<LogSink> make_standard_log_sink(const LogConfig& config);
#if PROTORELAY_HAS_SPDLOG
std::shared_ptr<LogSink> make_spdlog_log_sink(const LogConfig& config);
#endif
} // namespace pr
#endif
