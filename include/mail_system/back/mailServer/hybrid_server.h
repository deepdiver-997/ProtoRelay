#ifndef HYBRID_SERVER_H
#define HYBRID_SERVER_H

#include "server_base.h"
#include "session/generic_session_base.h"
#include "connection/connection_base.h"
#include <memory>
#include <map>

namespace mail_system {

// 混合模式服务器基类，可以同时支持SSL和非SSL连接
template<typename SSLSessionType, typename TCPSessionType>
class HybridServerBase : public ServerBase {
public:
    HybridServerBase(const ServerConfig& ssl_config, const ServerConfig& tcp_config)
        : ServerBase(ssl_config)  // 使用SSL配置初始化基类
        , ssl_config_(ssl_config)
        , tcp_config_(tcp_config)
        , ssl_enabled_(ssl_config.use_ssl)
        , tcp_enabled_(tcp_config.allow_insecure) {
        
        // 设置非SSL端口
        if (tcp_enabled_ && tcp_config_.insecure_port == 0) {
            // 如果没有指定非SSL端口，使用25端口
            tcp_config_.insecure_port = 25;
        }
    }

    virtual ~HybridServerBase() = default;

    // 启动混合服务器
    void start() {
        if (ssl_enabled_) {
            // 启动SSL监听
            ssl_acceptor_ = std::make_shared<boost::asio::ip::tcp::acceptor>(
                *get_io_context(),
                boost::asio::ip::tcp::endpoint(
                    boost::asio::ip::make_address(ssl_config_.address),
                    ssl_config_.port
                )
            );
            start_ssl_accept();
        }

        if (tcp_enabled_) {
            // 启动非SSL监听
            tcp_acceptor_ = std::make_shared<boost::asio::ip::tcp::acceptor>(
                *get_io_context(),
                boost::asio::ip::tcp::endpoint(
                    boost::asio::ip::make_address(tcp_config_.address),
                    tcp_config_.insecure_port
                )
            );
            start_tcp_accept();
        }
    }

    // 停止服务器
    void stop(ServerState state = ServerState::Pausing) {
        if (ssl_acceptor_) {
            ssl_acceptor_->close();
        }
        if (tcp_acceptor_) {
            tcp_acceptor_->close();
        }
        ServerBase::stop(state);
    }

protected:
#ifdef USE_SSL
    // 启动SSL接受
    void start_ssl_accept() {
        if (!ssl_enabled_) return;

        auto ssl_socket = std::make_unique<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>(
            *get_io_context(), get_ssl_context()
        );

        auto self = this->shared_from_this();
        ssl_acceptor_->async_accept(ssl_socket->lowest_layer(), 
            [this, self, socket = ssl_socket.get()](const boost::system::error_code& error) mutable {
                if (!error) {
                    std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> accepted_socket(socket);
                    handle_ssl_accept(std::move(accepted_socket), error);
                }
                start_ssl_accept();  // 继续接受下一个连接
            });
    }

    // 处理SSL连接（由派生类实现）
    virtual void handle_ssl_accept(std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>&& ssl_socket, 
                                   const boost::system::error_code& error) = 0;
#endif

    // 启动非SSL接受
    void start_tcp_accept() {
        if (!tcp_enabled_) return;

        auto tcp_socket = std::make_unique<boost::asio::ip::tcp::socket>(*get_io_context());

        auto self = this->shared_from_this();
        tcp_acceptor_->async_accept(*tcp_socket, 
            [this, self, socket = tcp_socket.get()](const boost::system::error_code& error) mutable {
                if (!error) {
                    std::unique_ptr<boost::asio::ip::tcp::socket> accepted_socket(socket);
                    handle_tcp_accept(std::move(accepted_socket), error);
                }
                start_tcp_accept();  // 继续接受下一个连接
            });
    }

    // 处理非SSL连接（由派生类实现）
    virtual void handle_tcp_accept(std::unique_ptr<boost::asio::ip::tcp::socket>&& tcp_socket, 
                                   const boost::system::error_code& error) = 0;

protected:
    ServerConfig ssl_config_;
    ServerConfig tcp_config_;
    bool ssl_enabled_;
    bool tcp_enabled_;

#ifdef USE_SSL
    std::shared_ptr<boost::asio::ip::tcp::acceptor> ssl_acceptor_;
#endif
    std::shared_ptr<boost::asio::ip::tcp::acceptor> tcp_acceptor_;
};

// 混合模式SMTPS服务器的示例实现
class HybridSMTPSServer : public HybridServerBase<SSLSessionBase, TCPSessionBase> {
public:
    HybridSMTPSServer(const ServerConfig& ssl_config, const ServerConfig& tcp_config)
        : HybridServerBase(ssl_config, tcp_config) {}

protected:
#ifdef USE_SSL
    // 实现SSL连接处理
    void handle_ssl_accept(std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>&& ssl_socket, 
                          const boost::system::error_code& error) override {
        if (error) {
            return;
        }

        // 创建SSL连接包装器
        auto ssl_connection = std::make_unique<SSLConnection>(std::move(ssl_socket));
        
        // 创建SSL会话
        auto session = std::make_shared<SSLSessionBase>(std::move(ssl_connection), this);
        session->start();
    }
#endif

    // 实现非SSL连接处理
    void handle_tcp_accept(std::unique_ptr<boost::asio::ip::tcp::socket>&& tcp_socket, 
                          const boost::system::error_code& error) override {
        if (error) {
            return;
        }

        // 创建TCP连接包装器
        auto tcp_connection = std::make_unique<TCPConnection>(std::move(tcp_socket));
        
        // 创建TCP会话
        auto session = std::make_shared<TCPSessionBase>(std::move(tcp_connection), this);
        session->start();
    }
};

} // namespace mail_system

#endif // HYBRID_SERVER_H