#ifndef SESSION_BASE_H
#define SESSION_BASE_H

#include "mail_system/back/common/logger.h"
#include "mail_system/back/mailServer/connection/i_connection.h"
#include "mail_system/back/mailServer/server_base.h"
#include "mail_system/back/entities/mail.h"
#include "mail_system/back/entities/usr.h"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mail_system {

class ServerBase;
class TcpConnection;
class SslConnection;
template <typename ConnectionType>
class SessionBase;
using TcpSessionBase = SessionBase<TcpConnection>;
using SslSessionBase = SessionBase<SslConnection>;

template <typename F>
auto make_copyable(F&& f) {
    auto s = std::make_shared<std::decay_t<F>>(std::forward<F>(f));
    return [s](auto&&... args) {
        return (*s)(std::forward<decltype(args)>(args)...);
    };
}

template <typename ConnectionType>
class SessionBase : public std::enable_shared_from_this<SessionBase<ConnectionType>> {
public:
    SessionBase(std::unique_ptr<ConnectionType> connection, ServerBase* server)
        : last_bytes_transferred_(0), stay_times_(0), timeout_times_(0)
        , connection_(std::move(connection))
        , read_buffer_(8192), use_buffer_(8192)
        , mail_(nullptr), closed_(false), session_authenticated_(false)
        , m_server(server)
        , session_start_(std::chrono::steady_clock::now()) {}

    virtual ~SessionBase() { if (!closed_) close(); }

    virtual void close() {
        if (closed_) return;
        closed_ = true;
        auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - session_start_).count();
        if (m_server) {
            MetricsServer::LabelMap lbls;
            lbls["auth"] = session_authenticated_ ? "yes" : "no";
            m_server->push_metric_observe("protorelay_session_duration_seconds", lbls, elapsed);
            m_server->record_session_end(get_client_ip(), session_authenticated_);
            m_server->decrement_connection_count();
        }

        // 将 pipeline 模式下积压的响应刷到连接再关闭
        if (!pending_write_buf_.empty() && connection_ && connection_->is_open()) {
            auto payload = std::make_shared<std::string>(std::move(pending_write_buf_));
            pending_write_buf_.clear();
            connection_->async_write(boost::asio::buffer(*payload),
                [payload](const boost::system::error_code&, std::size_t) {});
        }

        try {
            if (connection_ && connection_->is_open()) {
                connection_->close();
                LOG_SESSION_INFO("Session closed for {}", get_client_ip());
            }
        } catch (const std::exception& e) {
            LOG_SESSION_ERROR("Error closing session: {}", e.what());
        }
    }

    std::string get_client_ip() const {
        if (client_address_.empty() && connection_) {
            try { client_address_ = connection_->get_remote_ip(); }
            catch (const std::exception& e) { LOG_SESSION_ERROR("Error getting client IP: {}", e.what()); }
        }
        return client_address_;
    }

    ConnectionType& get_connection() { return *connection_; }
    const ConnectionType& get_connection() const { return *connection_; }
    std::unique_ptr<ConnectionType> release_connection() { return std::move(connection_); }

    mail* get_mail() { return mail_.get(); }
    std::unique_ptr<mail> get_mail_ptr() { return std::move(mail_); }
    usr* get_usr() { return usr_.get(); }
    ServerBase* get_server() const { return m_server; }
    void set_server(ServerBase* server) { m_server = server; }

    // 记录一次 AUTH 失败。返回 true 表示超过上限应关闭连接
    bool record_auth_failure_and_check() {
        auth_attempt_count_++;
        size_t max_attempts = 3;
        if (m_server) {
            auto cfg = std::atomic_load(&m_server->m_config);
            if (cfg) max_attempts = cfg->max_auth_attempts;
        }
        if (auth_attempt_count_ >= max_attempts) {
            LOG_SESSION_WARN("AUTH failures exceeded ({}/{}), closing session from {}",
                auth_attempt_count_, max_attempts, get_client_ip());
            return true;
        }
        return false;
    }

    virtual void handle_error(const boost::system::error_code& error) {
        LOG_SESSION_ERROR("SessionBase Error: {}", error.message());
        close();
    }

    bool is_closed() const { return closed_; }

    // SSL/TLS 握手（静态方法，在 session 创建阶段使用）
    template <typename HandshakeHandler>
    static void do_handshake(
        std::shared_ptr<SessionBase<ConnectionType>> self,
        boost::asio::ssl::stream_base::handshake_type type,
        HandshakeHandler&& handler)
    {
        if (self->closed_ || !self->connection_) return;
        auto* conn = self->connection_.get();
        conn->async_handshake(type,
            make_copyable([self, handler = std::forward<HandshakeHandler>(handler)](
                const boost::system::error_code& error) mutable {
                if (self->closed_) return;
                if (error) {
                    LOG_SESSION_ERROR("Handshake failed: {}", error.message());
                    self->handle_error(error);
                } else {
                    LOG_SESSION_INFO("Handshake successful.");
                }
                handler(self, error);
            }));
    }

    // ── 异步 I/O（成员函数） ──────────────────────────────────────────────
    using WriteCallback = std::function<void(std::shared_ptr<SessionBase<ConnectionType>>, const boost::system::error_code&)>;

    // 从命令缓冲区取出一条完整行（不含 \n 的残留数据留在缓冲区）
    std::string pop_buffered_line() {
        size_t nl = command_read_buffer_.find('\n');
        if (nl == std::string::npos) return "";
        std::string line = command_read_buffer_.substr(0, nl + 1);
        command_read_buffer_.erase(0, nl + 1);
        return line;
    }

    // 异步读：若命令缓冲区已有完整行则取出处理，否则发起网络读。
    // TCP 数据到达时先追加到缓冲区，然后逐行消费流水线命令。
    void do_async_read() {
        if (closed_ || !connection_) return;

        // 缓冲区已有完整行 → 返回，由外层 while 循环统一消费
        if (has_buffered_input()) return;

        auto* conn = connection_.get();
        auto buf = boost::asio::buffer(read_buffer_);

        conn->async_read(buf,
            make_copyable([self = this->shared_from_this()](
                const boost::system::error_code& error, std::size_t bytes) mutable {
                if (self->closed_) return;

                if (error) {
                    LOG_SESSION_ERROR("Error reading data: {}", error.message());
                    self->handle_error(error);
                    return;
                }

                if (bytes == 0) {
                    self->do_async_read();
                    return;
                }

                self->last_bytes_transferred_ = bytes;
                // 原始 TCP 数据追加到命令缓冲区（可能包含多行 + 不完整尾行）
                self->command_read_buffer_.append(
                    self->read_buffer_.data(), bytes);

                // 流水线消费：每次取全部缓冲数据（含 body 块），
                // parse_smtp_command 提取一行(非IN_MESSAGE)或处理body(IN_MESSAGE)
                while (self->has_buffered_input()) {
                    self->handle_read(self->take_buffered_input());
                    self->process_read();
                }
            }));
    }

    // 异步写：有流水线数据时批量累积 + 同步回调链处理后续命令，
    // 无流水线数据时真正写出（含之前累积的所有响应）。
    void do_async_write(const std::string& data, WriteCallback callback = nullptr) {
        if (closed_ || !connection_) return;

        // 还有流水线命令 → 累积响应，同步调用回调（回调中 do_async_read
        // 会取下一行 + process_read，链式消费直到缓冲区空）
        if (has_buffered_input()) {
            pending_write_buf_ += data;
            if (callback) {
                callback(this->shared_from_this(), boost::system::error_code());
            }
            return;
        }

        auto* conn = connection_.get();
        auto payload = std::make_shared<std::string>(
            std::move(pending_write_buf_) + data);
        conn->async_write_with_delay(boost::asio::buffer(*payload),
            compute_reply_delay(),
            [self = this->shared_from_this(), payload, cb = std::move(callback)](
                const boost::system::error_code& ec, std::size_t) mutable {
                if (self->closed_) return;
                if (ec) { self->handle_error(ec); return; }
                if (cb) cb(self, ec);
                else    self->do_async_read();
            });
    }

    // ── 纯虚接口 ─────────────────────────────────────────────────────────
    virtual void handle_read(const std::string& data) = 0;
    virtual void process_read() = 0;

    virtual bool has_buffered_input() const {
        return command_read_buffer_.find('\n') != std::string::npos;
    }
    virtual std::string take_buffered_input() {
        std::string s = std::move(command_read_buffer_);
        command_read_buffer_.clear();
        return s;
    }
    virtual std::chrono::milliseconds compute_reply_delay() const = 0;

    virtual void set_current_state(int state) = 0;
    virtual void set_next_event(int event) = 0;
    virtual void* get_fsm() const = 0;
    virtual int get_next_event() const = 0;
    virtual int get_current_state() const = 0;
    virtual std::string get_last_command_args() const = 0;
    virtual void* get_context() = 0;

    // 成员数据
    size_t last_bytes_transferred_;
    int stay_times_;
    int timeout_times_;

protected:
    std::unique_ptr<ConnectionType> connection_;
    std::vector<char> read_buffer_;
    std::vector<char> use_buffer_;
    std::string command_read_buffer_;
    std::string pending_write_buf_;
    mutable std::string client_address_;

public:
    std::unique_ptr<mail> mail_;
    std::unique_ptr<usr> usr_;

protected:
    bool closed_;
    bool session_authenticated_;
    int auth_attempt_count_ = 0;
    ServerBase* m_server;
    std::chrono::steady_clock::time_point session_start_;

public:
    void set_authenticated(bool v) { session_authenticated_ = v; }
    bool is_authenticated() const { return session_authenticated_; }
};

} // namespace mail_system

#endif // SESSION_BASE_H
