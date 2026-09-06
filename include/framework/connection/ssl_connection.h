#ifndef SSL_CONNECTION_H
#define SSL_CONNECTION_H

#include "i_connection.h"
#include <boost/asio/ssl.hpp>
#include <openssl/err.h>
#include <memory>

namespace mail_system {

// SSL 连接实现
class SslConnection : public IConnection {
public:
    SslConnection(
        std::unique_ptr<boost::asio::ip::tcp::socket> socket,
        boost::asio::ssl::context& ssl_context
    ) : stream_(
          std::make_unique<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>(
              std::move(*socket), ssl_context)) {}

    SslConnection(
        boost::asio::ssl::stream<boost::asio::ip::tcp::socket>&& stream
    ) : stream_(std::make_unique<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>(std::move(stream))) {}

    SslConnection(
        std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> stream
    ) : stream_(std::move(stream)) {}
    
    ~SslConnection() override = default;

    // 异步读取
    void async_read(
        boost::asio::mutable_buffer buffer,
        ReadHandler handler
    ) override {
        stream_->async_read_some(buffer, handler);
    }

    // 获取 executor
    boost::asio::any_io_executor get_executor() override {
        return stream_->get_executor();
    }

    // 异步写入
    void async_write(
        boost::asio::const_buffer buffer,
        WriteHandler handler
    ) override {
        boost::asio::async_write(*stream_, buffer, handler);
    }

    // SSL 握手
    void async_handshake(
        boost::asio::ssl::stream_base::handshake_type type,
        HandshakeHandler handler
    ) override {
        stream_->async_handshake(type, handler);
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

    // 释放原始 socket（从 SSL stream 中提取）
    std::unique_ptr<boost::asio::ip::tcp::socket> release_socket() override {
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
};

} // namespace mail_system

#endif // SSL_CONNECTION_H
