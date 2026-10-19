#include "framework/net/asio_connection.h"
#include "framework/connection/connection_handle.h"
#include "framework/net/asio_executor.h"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <openssl/err.h>

namespace pr {
namespace {
// TCP 连接实现
class AsioTcpConnection : public IConnection {
public:
    explicit AsioTcpConnection(std::unique_ptr<boost::asio::ip::tcp::socket> socket)
        : socket_(std::move(socket)), executor_(std::make_shared<net::AsioExecutor>(socket_->get_executor())) {}

    ~AsioTcpConnection() override = default;

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

    std::shared_ptr<net::AsioExecutor> asio_executor() const { return executor_; }

private:
    std::unique_ptr<boost::asio::ip::tcp::socket> socket_;
    std::shared_ptr<net::AsioExecutor> executor_;
};

// SSL 连接实现
class AsioSslConnection : public IConnection {
public:
    AsioSslConnection(
        std::unique_ptr<boost::asio::ip::tcp::socket> socket,
        std::shared_ptr<boost::asio::ssl::context> ssl_context,
        std::shared_ptr<net::AsioExecutor> executor = {}
    ) : stream_(net::asio_call("create TLS stream", [&] {
            return std::make_unique<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>(
                std::move(*socket), *ssl_context);
        })), executor_(executor ? std::move(executor) : std::make_shared<net::AsioExecutor>(stream_->get_executor())), context_(std::move(ssl_context)) {}

    ~AsioSslConnection() override = default;

    // 异步读取
    void async_read(
        net::MutableBuffer buffer,
        ReadHandler handler
    ) override {
        net::asio_call("TLS read", [&] {
            stream_->async_read_some(boost::asio::buffer(buffer.data(), buffer.size()),
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
        net::asio_call("TLS write", [&] {
            boost::asio::async_write(*stream_, boost::asio::buffer(buffer.data(), buffer.size()),
                boost::asio::bind_executor(executor_->native_executor(),
                    [handler = std::move(handler)](const boost::system::error_code& error, size_t size) {
                        handler(net::asio_error(error), size);
                    }));
        });
    }

    // SSL 握手
    void async_handshake(
        net::HandshakeRole type,
        HandshakeHandler handler
    ) override {
        net::asio_call("TLS handshake", [&] {
            stream_->async_handshake(type == net::HandshakeRole::server
                ? boost::asio::ssl::stream_base::server : boost::asio::ssl::stream_base::client,
                boost::asio::bind_executor(executor_->native_executor(),
                    [handler = std::move(handler)](const boost::system::error_code& error) {
                        handler(net::asio_error(error));
                    }));
        });
    }

    // 取消挂起的读/写（watchdog 用：只取消，不关闭连接）
    void cancel() override {
        if (!stream_) return;
        boost::system::error_code ec;
        stream_->lowest_layer().cancel(ec);
    }

    // 关闭连接
    void close() override {
        boost::system::error_code ec;
        // 不做同步 SSL_shutdown：它会阻塞 IO 线程等对端 close_notify，对端沉默
        // （手机 App 停靠连接 / 硬断开）就是无限 poll —— 09-05 17:29 全网断连
        // 根因（gdb 实锤两 IO 线程卡死于此）。直接关 TCP 层：该 socket 上排队的
        // ssl 多阶段 op 立刻以错误完成，其最外层完成回调持有会话 shared_ptr，
        // 会话必然活到 op 走完才析构（生命周期不变量，见 session_base.tpp close）。
        stream_->lowest_layer().close(ec);
    }

    // 检查连接是否打开
    bool is_open() const override {
        return stream_->lowest_layer().is_open();
    }

    // 获取本地端口
    uint16_t get_local_port() const override {
        boost::system::error_code ec;
        return stream_->lowest_layer().local_endpoint(ec).port();
    }

    // 获取远程 IP 地址
    std::string get_remote_ip() const override {
        boost::system::error_code ec;
        return stream_->lowest_layer().remote_endpoint(ec).address().to_string();
    }

    // Asio-only transport handoff extension; not part of IConnection.
    std::unique_ptr<boost::asio::ip::tcp::socket> release_socket() {
        return std::make_unique<boost::asio::ip::tcp::socket>(
            std::move(stream_->next_layer())
        );
    }

    // 握手失败诊断：输出 SSL 状态 + OpenSSL 错误栈（比裸 "Connection reset by peer" 详细）
    std::string get_handshake_diagnostic() const override {
        std::string out;
        SSL* ssl = stream_->native_handle();
        if (ssl) {
            out += "ssl_state=";
            out += SSL_state_string_long(ssl);
            const char* ver = SSL_get_version(ssl);
            if (ver && *ver) { out += " version="; out += ver; }
        }
        unsigned long err = ERR_get_error();
        if (err) {
            out += " openssl=[";
            char buf[256];
            bool first = true;
            while (err) {
                if (!first) out += "; ";
                ERR_error_string_n(err, buf, sizeof(buf));
                out += buf;
                first = false;
                err = ERR_get_error();
            }
            out += "]";
        }
        return out;
    }

private:
    std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> stream_;
    std::shared_ptr<net::AsioExecutor> executor_;
    std::shared_ptr<boost::asio::ssl::context> context_;
};

} // namespace
namespace net {
std::unique_ptr<IConnection> make_asio_tcp_connection(std::unique_ptr<boost::asio::ip::tcp::socket> socket) {
    return std::make_unique<AsioTcpConnection>(std::move(socket));
}
std::unique_ptr<IConnection> make_asio_ssl_connection(
    std::unique_ptr<boost::asio::ip::tcp::socket> socket,
    std::shared_ptr<boost::asio::ssl::context> context) {
    return std::make_unique<AsioSslConnection>(std::move(socket), std::move(context));
}
std::unique_ptr<IConnection> upgrade_asio_tls(std::unique_ptr<IConnection> connection,
    std::shared_ptr<boost::asio::ssl::context> context) {
    connection = take_connection_transport(std::move(connection));
    auto* tcp = dynamic_cast<AsioTcpConnection*>(connection.get());
    if (!tcp) throw std::system_error(std::make_error_code(std::errc::operation_not_supported), "TLS upgrade requires an Asio TCP connection");
    auto executor = tcp->asio_executor();
    auto socket = tcp->release_socket();
    return std::make_unique<AsioSslConnection>(std::move(socket), std::move(context), std::move(executor));
}
} // namespace net
} // namespace pr
