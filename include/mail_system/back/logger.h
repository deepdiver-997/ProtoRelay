#ifndef LOGGER_H_
#define LOGGER_H_

#include <boost/log/core.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <boost/log/sinks/async_frontend.hpp>

namespace mail_system {

class Logger {
public:
    Logger();
    ~Logger();

    void init();
    void log(const std::string& message){
        BOOST_LOG_TRIVIAL(info) << message;
    }

private:
};
Logger::Logger() {
    init();
}

Logger::~Logger() {
    boost::log::core::get()->remove_all_sinks();
}

void Logger::init() {
    namespace keywords = boost::log::keywords;
    boost::log::add_file_log(
        keywords::file_name = "/home/vitali/Desktop/mailer/mail_system/logs/log_%N.log",
        keywords::rotation_size = 100 * 1024 * 1024,
        keywords::auto_flush = true);
}
} // namespace mail_system

#endif // LOGGER_H_