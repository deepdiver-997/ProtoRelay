#ifndef GENERIC_SESSION_BASE_H
#define GENERIC_SESSION_BASE_H

#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <boost/asio.hpp>
#ifdef USE_SSL
#include <boost/asio/ssl.hpp>
#endif
#include "mail_system/back/entities/mail.h"
#include "mail_system/back/entities/usr.h"
#include "mail_system/back/mailServer/connection/connection_base.h"
#include "mail_system/back/mailServer/server_base.h"

namespace mail_system {

// 前向声明
template<typename ConnectionType>
class GenericSessionBase;

// 类型别名定义
#ifdef USE_SSL
using SSLSessionBase = GenericSessionBase<SSLConnection>;
#endif
using TCPSessionBase = GenericSessionBase<TCPConnection>;

// 泛型Session基类模板
template<typename ConnectionType>
class GenericSessionBase : public std::enable_shared_from_this<GenericSessionBase<ConnectionType>> {
public:
    // 构造函数
    GenericSessionBase(std::unique_ptr<ConnectionType>&& connection, ServerBase* server)
        : connection_(std::move(connection))
        , read_buffer_(8192)  // 8KB缓冲区
        , closed_(false)
        , m_server(server) {}

    // 虚析构函数
    virtual ~GenericSessionBase() {
        if (!closed_) {
            close();
        }
    }

    // 启动会话（纯虚函数，由派生类实现）
    virtual void start() = 0;

    // 关闭会话
    virtual void close() {
        if (closed_) {
            return;
        }
        
        closed_ = true;
        
        if (connection_ && connection_->is_open()) {
            connection_->close();
        }
    }

    // 获取客户端地址
    std::string get_client_ip() const {
        if (client_address_.empty() && connection_) {
            std::string endpoint = connection_->get_remote_endpoint();
            // 提取IP部分（去掉端口号）
            size_t colon_pos = endpoint.find(':');
            if (colon_pos != std::string::npos) {
                client_address_ = endpoint.substr(0, colon_pos);
            } else {
                client_address_ = endpoint;
            }
        }
        return client_address_;
    }

    // 获取连接对象
    ConnectionType& get_connection() {
        return *connection_;
    }

    // 获取连接对象（常量版本）
    const ConnectionType& get_connection() const {
        return *connection_;
    }

    // 异步读取数据
    void async_read(std::function<void(const boost::system::error_code&, std::size_t)> callback = nullptr) {
        if (closed_) {
            return;
        }

        auto self = this->shared_from_this();
        auto read_callback = [this, self, callback](const boost::system::error_code& error, std::size_t bytes_transferred) {
            if (closed_) {
                return;
            }

            if (error) {
                handle_error(error);
                if (callback) {
                    callback(error, bytes_transferred);
                }
                return;
            }

            // 处理接收到的数据
            std::string data(read_buffer_.data(), bytes_transferred);
            handle_read(data);

            if (callback) {
                callback(error, bytes_transferred);
            }
        };

        connection_->async_read(boost::asio::buffer(read_buffer_), read_callback);
    }

    // 异步写入数据
    void async_write(const std::string& data, std::function<void(const boost::system::error_code&)> callback = nullptr) {
        if (closed_) {
            return;
        }

        auto self = this->shared_from_this();
        auto write_callback = [this, self, callback](const boost::system::error_code& error, std::size_t) {
            if (closed_) {
                return;
            }

            if (error) {
                handle_error(error);
            }

            if (callback) {
                callback(error);
            }
        };

        connection_->async_write(boost::asio::buffer(data), write_callback);
    }

    // 处理接收到的数据（纯虚函数，由派生类实现）
    virtual void handle_read(const std::string& data) = 0;

    // 处理错误（虚函数，派生类可以重写）
    virtual void handle_error(const boost::system::error_code& error) {
        if (error == boost::asio::error::eof ||
            error == boost::asio::error::connection_reset ||
            error == boost::asio::error::operation_aborted) {
            // 正常的连接关闭或取消操作
        } else {
            // 其他错误
        }
        close();
    }

    // 会话是否已关闭
    bool is_closed() const {
        return closed_;
    }

    // 获取邮件对象
    mail* get_mail() {
        return mail_.get();
    }

    // 获取用户对象
    usr* get_usr() {
        return usr_.get();
    }

    // 获取服务器指针
    ServerBase* get_server() const {
        return m_server;
    }

    // 设置服务器引用
    void set_server(ServerBase* server) {
        m_server = server;
    }

#ifdef USE_SSL
    // SSL握手（仅SSL连接可用）
    template<typename HandshakeHandler>
    void do_handshake(boost::asio::ssl::stream_base::handshake_type type, HandshakeHandler&& handler) {
        if constexpr (std::is_same_v<ConnectionType, SSLConnection>) {
            auto self = this->shared_from_this();
            connection_->async_handshake(type, [this, self, handler](const boost::system::error_code& error) {
                if (error) {
                    handle_error(error);
                }
                handler(std::static_pointer_cast<GenericSessionBase<ConnectionType>>(self), error);
            });
        }
    }
#endif

protected:
    // 连接对象
    std::unique_ptr<ConnectionType> connection_;

    // 读取缓冲区
    std::vector<char> read_buffer_;

    // 客户端地址缓存
    mutable std::string client_address_;

    // 邮件和用户对象
    std::unique_ptr<mail> mail_;
    std::unique_ptr<usr> usr_;

    // 会话是否已关闭
    bool closed_;

    // 指向服务器的指针
    ServerBase* m_server;
};

} // namespace mail_system

#endif // GENERIC_SESSION_BASE_H