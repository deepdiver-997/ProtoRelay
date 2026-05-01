#ifndef MAIL_SYSTEM_METRICS_SERVER_H
#define MAIL_SYSTEM_METRICS_SERVER_H

#include <boost/asio.hpp>
#include <functional>
#include <memory>
#include <string>

namespace mail_system {

// 最小化 HTTP 服务器，仅响应 GET /metrics 和 /health
// 只用 Boost.Asio，不引入 beast 等额外依赖
// 复用外部 io_context，不创建额外线程
class MetricsServer {
public:
    using MetricsProvider = std::function<std::string()>;

    MetricsServer(boost::asio::io_context& io_ctx,
                  uint16_t port, const std::string& bind_address,
                  MetricsProvider provider)
        : io_ctx_(io_ctx)
        , acceptor_(io_ctx_)
        , port_(port)
        , bind_address_(bind_address)
        , provider_(std::move(provider))
        , running_(false) {}

    ~MetricsServer() { stop(); }

    MetricsServer(const MetricsServer&) = delete;
    MetricsServer& operator=(const MetricsServer&) = delete;

    void start() {
        if (running_.exchange(true)) {
            return;
        }

        boost::asio::ip::tcp::endpoint ep(
            boost::asio::ip::make_address(bind_address_), port_);
        acceptor_.open(ep.protocol());
        acceptor_.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
        acceptor_.bind(ep);
        acceptor_.listen();

        do_accept();
    }

    void stop() {
        if (!running_.exchange(false)) {
            return;
        }

        boost::system::error_code ec;
        acceptor_.close(ec);
    }

private:
    void do_accept() {
        auto socket = std::make_shared<boost::asio::ip::tcp::socket>(io_ctx_);
        acceptor_.async_accept(*socket, [this, socket](const boost::system::error_code& ec) {
            if (!ec) {
                handle_request(socket);
            }
            if (running_.load()) {
                do_accept();
            }
        });
    }

    void handle_request(std::shared_ptr<boost::asio::ip::tcp::socket> socket) {
        auto buf = std::make_shared<boost::asio::streambuf>();
        boost::asio::async_read_until(*socket, *buf, "\r\n\r\n",
            [this, socket, buf](const boost::system::error_code& ec, std::size_t) {
                if (ec) {
                    return;
                }

                std::string request;
                std::istream is(buf.get());
                std::getline(is, request); // 只读第一行 "GET /metrics HTTP/1.1"

                std::string path;
                size_t start = request.find(' ');
                size_t end = request.find(' ', start + 1);
                if (start != std::string::npos && end != std::string::npos) {
                    path = request.substr(start + 1, end - start - 1);
                }

                std::string status;
                std::string content_type;
                std::string body;

                if (path == "/metrics" || path == "/") {
                    status = "200 OK";
                    content_type = "text/plain; version=0.0.4";
                    body = provider_ ? provider_() : "";
                } else if (path == "/health" || path == "/health/live") {
                    status = "200 OK";
                    content_type = "text/plain";
                    body = "OK";
                } else if (path == "/health/ready") {
                    // 准备就绪检查：provider 返回非空表示正常
                    body = provider_ ? provider_() : "";
                    if (!body.empty()) {
                        status = "200 OK";
                    } else {
                        status = "503 Service Unavailable";
                    }
                    content_type = "text/plain";
                } else {
                    status = "404 Not Found";
                    content_type = "text/plain";
                    body = "Not Found";
                }

                auto response = std::make_shared<std::string>();
                response->reserve(256 + body.size());
                response->append("HTTP/1.1 ").append(status).append("\r\n");
                response->append("Content-Type: ").append(content_type).append("\r\n");
                response->append("Content-Length: ").append(std::to_string(body.size())).append("\r\n");
                response->append("Connection: close\r\n");
                response->append("\r\n");
                response->append(body);

                boost::asio::async_write(*socket, boost::asio::buffer(*response),
                    [socket, response](const boost::system::error_code&, std::size_t) {
                        boost::system::error_code ignored;
                        socket->shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
                    });
            });
    }

    boost::asio::io_context& io_ctx_;
    boost::asio::ip::tcp::acceptor acceptor_;
    uint16_t port_;
    std::string bind_address_;
    MetricsProvider provider_;
    std::atomic<bool> running_;
};

} // namespace mail_system

#endif // MAIL_SYSTEM_METRICS_SERVER_H
