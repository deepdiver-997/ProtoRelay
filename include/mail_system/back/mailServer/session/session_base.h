#ifndef SESSION_BASE_H
#define SESSION_BASE_H

#include "mail_system/back/common/logger.h"
#include "mail_system/back/mailServer/connection/i_connection.h"
// #include "mail_system/back/mailServer/connection/tcp_connection.h"
#include "mail_system/back/mailServer/server_base.h"
#include "mail_system/back/entities/mail.h"
#include "mail_system/back/entities/usr.h"
#include <boost/asio.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mail_system {

// 前向声明
class ServerBase;
class TcpConnection;
class SslConnection;
template <typename ConnectionType>
class SessionBase;
// 类型别名
using TcpSessionBase = SessionBase<TcpConnection>;
using SslSessionBase = SessionBase<SslConnection>;

// 辅助函数：将只可移动的函数对象（如捕获了 unique_ptr 的 lambda）包装为可复制的
// 这样就可以传递给 std::function
template <typename F>
auto make_copyable(F&& f) {
    auto s = std::make_shared<std::decay_t<F>>(std::forward<F>(f));
    return [s](auto&&... args) {
        return (*s)(std::forward<decltype(args)>(args)...);
    };
}

// Session 基类模板
template <typename ConnectionType>
class SessionBase {
public:
    // 构造函数
    SessionBase(std::unique_ptr<ConnectionType> connection, ServerBase* server)
        : last_bytes_transferred_(0)
        , stay_times_(0)
        , timeout_times_(0)
        , connection_(std::move(connection))
        , read_buffer_(8192)
        , use_buffer_(8192)
        , mail_(nullptr)
        , closed_(false)
        , m_server(server) {}

    // 移动构造函数
    SessionBase(SessionBase&& other) noexcept
        : last_bytes_transferred_(other.last_bytes_transferred_)
        , stay_times_(other.stay_times_)
        , timeout_times_(other.timeout_times_)
        , connection_(std::move(other.connection_))
        , read_buffer_(std::move(other.read_buffer_))
        , use_buffer_(std::move(other.use_buffer_))
        , client_address_(std::move(other.client_address_))
        , mail_(std::move(other.mail_))
        , usr_(std::move(other.usr_))
        , closed_(other.closed_)
        , m_server(other.m_server) {
        other.closed_ = true;
        other.m_server = nullptr;
    }

    // 虚析构函数
    virtual ~SessionBase() {
        if (!closed_) {
            close();
        }
    }

    // 关闭会话
    virtual void close() {
        if (closed_) {
            return;
        }

        closed_ = true;

        if (m_server) {
            m_server->decrement_connection_count();
        }

        try {
            if (connection_ && connection_->is_open()) {
                connection_->close();
                std::cout << "Session closed for " << get_client_ip() << std::endl;
            } else {
                std::cout << "Session already closed or connection not open." << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error closing session: " << e.what() << std::endl;
        }
    }

    // 获取客户端地址
    std::string get_client_ip() const {
        if (client_address_.empty()) {
            try {
                client_address_ = connection_->get_remote_ip();
            } catch (const std::exception& e) {
                std::cerr << "Error getting client IP: " << e.what() << std::endl;
            }
        }
        return client_address_;
    }

    // 获取连接对象
    ConnectionType& get_connection() {
        if (!connection_) {
            throw std::runtime_error("Connection is not initialized");
        }
        return *connection_;
    }

    // 获取连接对象（常量版本）
    const ConnectionType& get_connection() const {
        if (!connection_) {
            throw std::runtime_error("Connection is not initialized");
        }
        return *connection_;
    }

    std::unique_ptr<ConnectionType> release_connection() {
        return std::move(connection_);
    }

    // 获取邮件对象
    mail* get_mail() { return mail_.get(); }

    // 获取邮件对象指针（转移所有权）
    std::unique_ptr<mail> get_mail_ptr() { return std::move(mail_); }

    // 获取用户对象
    usr* get_usr() { return usr_.get(); }

    // 获取服务器指针
    ServerBase* get_server() const { return m_server; }

    // 设置服务器引用
    void set_server(ServerBase* server) { m_server = server; }

    // 执行握手（静态方法，传入 self）
    template <typename HandshakeHandler>
    static void do_handshake(
        std::unique_ptr<SessionBase<ConnectionType>> self,
        boost::asio::ssl::stream_base::handshake_type type,
        HandshakeHandler&& handler
    ) {
        if (self->closed_ || !self->connection_) {
            return;
        }

        auto* conn = self->connection_.get();

        conn->async_handshake(
            type,
            make_copyable([self = std::move(self), handler = std::forward<HandshakeHandler>(handler)](const boost::system::error_code& error) mutable {
                if (self->closed_) {
                    return;
                }

                if (error) {
                    LOG_SESSION_ERROR("Handshake failed: {}", error.message());
                    self->handle_error(error);
                } else {
                    LOG_SESSION_INFO("Handshake successful.");
                }

                handler(std::move(self), error);
            })
        );
    }

    // 处理错误（虚函数，派生类可以重写）
    virtual void handle_error(const boost::system::error_code& error) {
        LOG_SESSION_ERROR("SessionBase Error: {}", error.message());
        close();
    }

    // 会话是否已关闭
    bool is_closed() const { return closed_; }

    // 获取最后读取的数据（用于状态机回调）
    std::string get_last_read_data(size_t bytes_transferred) {
        auto s = std::string(read_buffer_.data(), bytes_transferred);
        std::fill(read_buffer_.begin(), read_buffer_.end(), 0);
        return s;
    }

    // 最近一次异步读取传输的字节数
    size_t last_bytes_transferred_;

    // 会话统计信息
    int stay_times_;
    int timeout_times_;

protected:
    // 连接对象
    std::unique_ptr<ConnectionType> connection_;

    // 读取缓冲区
    std::vector<char> read_buffer_;
    std::vector<char> use_buffer_;

    // 客户端地址缓存
    mutable std::string client_address_;

public:
    // 邮件和用户对象
    std::unique_ptr<mail> mail_;
    std::unique_ptr<usr> usr_;

protected:
    // 会话是否已关闭
    bool closed_;

    // 指向服务器的指针，用于访问 IO 线程池
    ServerBase* m_server;

public:
    // 回调类型定义
    using ReadCallback = std::function<void(
        std::unique_ptr<SessionBase<ConnectionType>>,
        const boost::system::error_code&,
        std::size_t
    )>&&;

    using WriteCallback = std::function<void(
        std::unique_ptr<SessionBase<ConnectionType>>,
        const boost::system::error_code&
    )>&&;

    // 静态异步读取（回调为空时自动调用 process_read）
    static void do_async_read(
        std::unique_ptr<SessionBase<ConnectionType>> self,
        ReadCallback callback = nullptr
    ) {
        if (self->closed_ || !self->connection_) {
            return;
        }

        auto* conn = self->connection_.get();
        auto read_buffer = boost::asio::buffer(self->read_buffer_);

        conn->async_read(
            read_buffer,
            make_copyable([self = std::move(self), cb = std::move(callback)](
                const boost::system::error_code& error,
                std::size_t bytes_transferred
            ) mutable {
                if (self->closed_) {
                    return;
                }

                if (error) {
                    LOG_SESSION_ERROR("Error reading data: {}", error.message());
                    self->handle_error(error);
                    return;
                }

                // 0 字节：忽略并继续等待
                if (bytes_transferred == 0) {
                    std::cout << "No data read, continue waiting..." << std::endl;
                    do_async_read(std::move(self), std::move(cb));
                    return;
                }

                // 记录本次读取的字节数
                self->last_bytes_transferred_ = bytes_transferred;

                // 处理接收到的数据
                std::string data(self->read_buffer_.data(), bytes_transferred);
                self->handle_read(data);

                // 回调不为空则执行回调
                if (cb) {
                    // 直接在IO线程中执行回调，避免不必要的线程切换
                    cb(std::move(self), error, bytes_transferred);
                }
                else {
                    // 默认处理：调用process_read
                    self->process_read(std::move(self));
                }
            })
        );
    }

    // 静态异步写入（回调为空时自动继续读取）
    static void do_async_write(
        std::unique_ptr<SessionBase<ConnectionType>> self,
        const std::string& data,
        WriteCallback callback = nullptr
    ) {
        if (self->closed_ || !self->connection_) {
            return;
        }

        auto* conn = self->connection_.get();

        // Keep payload alive until async completion to avoid dangling buffer.
        auto payload = std::make_shared<std::string>(data);
        auto write_buffer = boost::asio::buffer(*payload);

        auto delay = self->compute_reply_delay();

        conn->async_write_with_delay(
            write_buffer,
            delay,
            make_copyable([self = std::move(self), cb = std::move(callback), payload](
                const boost::system::error_code& error,
                std::size_t
            ) mutable {
                if (self->closed_) {
                    return;
                }

                if (error) {
                    std::cerr << "Error writing data: " << error.message() << std::endl;
                    self->handle_error(error);
                } else {
                    if (cb) {
                        // 直接在IO线程中执行回调
                        cb(std::move(self), error);
                    } else {
                        // 回调为空，自动继续读取
                        do_async_read(std::move(self), nullptr);
                    }
                }
            })
        );
    }

    // 纯虚函数，由派生类实现
    virtual void handle_read(const std::string& data) = 0;
    virtual void process_read(std::unique_ptr<SessionBase<ConnectionType>> self) = 0;

    // 计算当前回复延迟（子类根据负载实现，0ms = 无延迟）
    virtual std::chrono::milliseconds compute_reply_delay() const = 0;

    // 状态机相关接口（纯虚函数，由派生类实现）
    virtual void set_current_state(int state) = 0;
    virtual void set_next_event(int event) = 0;
    virtual void* get_fsm() const = 0;
    virtual int get_next_event() const = 0;
    virtual int get_current_state() const = 0;
    virtual std::string get_last_command_args() const = 0;
    virtual void* get_context() = 0;
};

} // namespace mail_system

#endif // SESSION_BASE_H
