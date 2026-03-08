#ifndef MAIL_SYSTEM_SMTP_TRANSPORT_UTILS_H
#define MAIL_SYSTEM_SMTP_TRANSPORT_UTILS_H

#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>

#include <chrono>
#include <cctype>
#include <functional>
#include <string>
#include <thread>

namespace mail_system {
namespace outbound {
namespace smtp_transport {

using ContinueFn = std::function<bool()>;

constexpr auto kIoPollInterval = std::chrono::milliseconds(10);
constexpr auto kIoOperationTimeout = std::chrono::seconds(20);

inline std::string trim_cr(std::string line) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

inline bool parse_smtp_code(const std::string& line, int& code_out) {
    if (line.size() < 3 ||
        !std::isdigit(static_cast<unsigned char>(line[0])) ||
        !std::isdigit(static_cast<unsigned char>(line[1])) ||
        !std::isdigit(static_cast<unsigned char>(line[2]))) {
        return false;
    }

    code_out = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
    return true;
}

inline bool looks_like_ehlo_capability_response(const std::string& response) {
    if (response.rfind("250-", 0) != 0) {
        return false;
    }
    return response.find("SMTPUTF8") != std::string::npos ||
           response.find("SIZE") != std::string::npos ||
           response.find("8BITMIME") != std::string::npos ||
           response.find("STARTTLS") != std::string::npos;
}

template <typename StreamType>
auto& lowest_layer(StreamType& stream) {
    return stream;
}

template <typename NextLayer>
auto& lowest_layer(boost::asio::ssl::stream<NextLayer>& stream) {
    return stream.lowest_layer();
}

template <typename StreamType>
void abort_stream(StreamType& stream) {
    auto& layer = lowest_layer(stream);
    boost::system::error_code ignored;
    layer.cancel(ignored);
    layer.close(ignored);
}

template <typename StreamType, typename StartOp>
bool run_interruptible_ec(StreamType& stream,
                          StartOp&& start_op,
                          const ContinueFn& should_continue,
                          std::chrono::milliseconds timeout,
                          const char* op_name,
                          std::string& error_out) {
    auto& io = static_cast<boost::asio::io_context&>(lowest_layer(stream).get_executor().context());
    boost::system::error_code op_ec = boost::asio::error::would_block;
    bool done = false;

    start_op([&](const boost::system::error_code& ec) {
        op_ec = ec;
        done = true;
    });

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done) {
        if (should_continue && !should_continue()) {
            abort_stream(stream);
            error_out = std::string(op_name) + " interrupted";
            return false;
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            abort_stream(stream);
            error_out = std::string(op_name) + " timeout";
            return false;
        }

        io.restart();
        io.poll();
        std::this_thread::sleep_for(kIoPollInterval);
    }

    if (op_ec) {
        error_out = std::string(op_name) + " failed: " + op_ec.message();
        return false;
    }

    return true;
}

template <typename StreamType, typename StartOp>
bool run_interruptible_ec_size(StreamType& stream,
                               StartOp&& start_op,
                               const ContinueFn& should_continue,
                               std::chrono::milliseconds timeout,
                               const char* op_name,
                               std::string& error_out) {
    auto& io = static_cast<boost::asio::io_context&>(lowest_layer(stream).get_executor().context());
    boost::system::error_code op_ec = boost::asio::error::would_block;
    bool done = false;

    start_op([&](const boost::system::error_code& ec, std::size_t) {
        op_ec = ec;
        done = true;
    });

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done) {
        if (should_continue && !should_continue()) {
            abort_stream(stream);
            error_out = std::string(op_name) + " interrupted";
            return false;
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            abort_stream(stream);
            error_out = std::string(op_name) + " timeout";
            return false;
        }

        io.restart();
        io.poll();
        std::this_thread::sleep_for(kIoPollInterval);
    }

    if (op_ec) {
        error_out = std::string(op_name) + " failed: " + op_ec.message();
        return false;
    }

    return true;
}

template <typename StreamType>
bool read_line(StreamType& stream,
               boost::asio::streambuf& buffer,
               std::string& line_out,
               const ContinueFn& should_continue,
               std::string& error_out) {
    while (true) {
        std::istream is(&buffer);
        if (std::getline(is, line_out)) {
            line_out = trim_cr(line_out);
            return true;
        }

        if (!run_interruptible_ec_size(
                stream,
                [&](auto handler) { boost::asio::async_read_until(stream, buffer, "\n", std::move(handler)); },
                should_continue,
                kIoOperationTimeout,
                "read smtp response line",
                error_out)) {
            return false;
        }
    }
}

template <typename StreamType>
bool read_smtp_response(StreamType& stream,
                        boost::asio::streambuf& buffer,
                        int& code_out,
                        std::string& response_out,
                        const ContinueFn& should_continue,
                        std::string& error_out) {
    response_out.clear();
    code_out = 0;

    std::string line;
    if (!read_line(stream, buffer, line, should_continue, error_out)) {
        return false;
    }
    response_out = line;

    if (!parse_smtp_code(line, code_out)) {
        error_out = "invalid smtp response: " + line;
        return false;
    }

    while (line.size() >= 4 && line[3] == '-') {
        if (!read_line(stream, buffer, line, should_continue, error_out)) {
            error_out = "truncated multi-line smtp response: " + error_out;
            return false;
        }
        response_out += "\n" + line;
    }

    return true;
}

template <typename StreamType>
bool send_smtp_line(StreamType& stream,
                    const std::string& line,
                    const ContinueFn& should_continue,
                    std::string& error_out) {
    const std::string wire = line + "\r\n";
    return run_interruptible_ec_size(
        stream,
        [&](auto handler) { boost::asio::async_write(stream, boost::asio::buffer(wire), std::move(handler)); },
        should_continue,
        kIoOperationTimeout,
        "write smtp line",
        error_out);
}

template <typename StreamType>
bool send_smtp_data(StreamType& stream,
                    const std::string& data,
                    const ContinueFn& should_continue,
                    std::string& error_out) {
    return run_interruptible_ec_size(
        stream,
        [&](auto handler) { boost::asio::async_write(stream, boost::asio::buffer(data), std::move(handler)); },
        should_continue,
        kIoOperationTimeout,
        "write smtp body",
        error_out);
}

} // namespace smtp_transport
} // namespace outbound
} // namespace mail_system

#endif // MAIL_SYSTEM_SMTP_TRANSPORT_UTILS_H
