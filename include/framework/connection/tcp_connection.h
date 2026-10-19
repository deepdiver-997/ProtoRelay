#ifndef PR_FRAMEWORK_TCP_CONNECTION_H
#define PR_FRAMEWORK_TCP_CONNECTION_H

#include "i_connection.h"
#include "framework/net/asio_executor.h"
#include <boost/asio.hpp>
#include <memory>

namespace pr {

// TCP 连接实现
class TcpConnection : public IConnection {
public:
    explicit TcpConnection(std::unique_ptr<boost::asio::ip::tcp::socket> socket)
        : socket_(std::move(socket)), executor_(std::make_shared<net::AsioExecutor>(socket_->get_executor())) {}

    ~TcpConnection() override = default;

    // 异步读取
    void async_read(
        net::MutableBuffer buffer,
        ReadHandler handler
    ) override {
        net::asio_call("TCP read", [&] {
            socket_->async_read_some(boost::asio::buffer(buffer.data(), buffer.size()),
                boost::asio::bind_executor(executor_->native_executor(),
                    [handler = std::move(handler)](const boost::system::error_code& error, size_t size) {
                        handler(net::asio_error(error), size);
                    }));
        });
    }

    // 获取 executor
    std::shared_ptr<net::IExecutor> executor() override { return executor_; }

    // 异步写入
    void async_write(
        net::ConstBuffer buffer,
        WriteHandler handler
    ) override {
        net::asio_call("TCP write", [&] {
            boost::asio::async_write(*socket_, boost::asio::buffer(buffer.data(), buffer.size()),
                boost::asio::bind_executor(executor_->native_executor(),
                    [handler = std::move(handler)](const boost::system::error_code& error, size_t size) {
                        handler(net::asio_error(error), size);
                    }));
        });
    }

    // TCP needs no TLS handshake; still dispatch completion asynchronously.
    void async_handshake(
        net::HandshakeRole,
        HandshakeHandler handler
    ) override {
        net::asio_call("TCP handshake", [&] {
            executor_->post([handler = std::move(handler)] { handler({}); });
        });
    }

    // 关闭连接
    void close() override {
        boost::system::error_code ec;
        socket_->close(ec);
    }

    // 检查连接是否打开
    bool is_open() const override {
        return socket_->is_open();
    }

    // 获取本地端口
    uint16_t get_local_port() const override {
        boost::system::error_code ec;
        return socket_->local_endpoint(ec).port();
    }

    // 获取远程 IP 地址
    std::string get_remote_ip() const override {
        boost::system::error_code ec;
        return socket_->remote_endpoint(ec).address().to_string();
    }

    // 原生 fd（零拷贝 sendfile 用；仅限明文路径）
    int native_handle() const override {
        return socket_ ? socket_->native_handle() : -1;
    }

    // 取消挂起的读/写 → 其 handler 以 operation_aborted 返回（watchdog 用）
    void cancel() override {
        if (!socket_) return;
        boost::system::error_code ec;
        socket_->cancel(ec);
    }

    // Asio-only STARTTLS extension; deliberately absent from IConnection.
    std::unique_ptr<boost::asio::ip::tcp::socket> release_socket() {
        return std::move(socket_);
    }

private:
    std::unique_ptr<boost::asio::ip::tcp::socket> socket_;
    std::shared_ptr<net::AsioExecutor> executor_;
};

} // namespace pr

#endif // PR_FRAMEWORK_TCP_CONNECTION_H
