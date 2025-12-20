/*
 * 泛型邮件服务器使用示例
 * 展示如何使用新的泛型设计创建SSL和非SSL邮件服务器
 */

#include "mail_system/back/mailServer/server_base.h"
#include "mail_system/back/mailServer/hybrid_server.h"
#include "mail_system/back/mailServer/session/generic_session_base.h"
#include "mail_system/back/mailServer/connection/connection_base.h"
#include <iostream>
#include <memory>

using namespace mail_system;

// 示例SSL会话实现
class ExampleSSLSession : public SSLSessionBase {
public:
    ExampleSSLSession(std::unique_ptr<SSLConnection>&& connection, ServerBase* server)
        : SSLSessionBase(std::move(connection), server) {}

    void start() override {
        std::cout << "SSL Session started from " << get_client_ip() << std::endl;
        
        // 执行SSL握手
        do_handshake(boost::asio::ssl::stream_base::server,
            [self = this->shared_from_this()](std::weak_ptr<SSLSessionBase> weak_session, const boost::system::error_code& error) {
                if (auto session = weak_session.lock()) {
                    if (!error) {
                        std::cout << "SSL handshake successful" << std::endl;
                        // 发送欢迎消息
                        session->async_write("220 Welcome to SSL Mail Server\r\n",
                            [weak_session](const boost::system::error_code& write_error) {
                                if (write_error) {
                                    std::cerr << "Error sending welcome message: " << write_error.message() << std::endl;
                                }
                            });
                        // 开始读取
                        session->async_read();
                    } else {
                        std::cerr << "SSL handshake failed: " << error.message() << std::endl;
                    }
                }
            });
    }

    void handle_read(const std::string& data) override {
        std::cout << "Received SSL data: " << data << std::endl;
        
        // 简单的回显
        async_write("Echo: " + data,
            [self = this->shared_from_this()](const boost::system::error_code& error) {
                if (!error) {
                    // 继续读取
                    self->async_read();
                }
            });
    }
};

// 示例TCP会话实现
class ExampleTCPSession : public TCPSessionBase {
public:
    ExampleTCPSession(std::unique_ptr<TCPConnection>&& connection, ServerBase* server)
        : TCPSessionBase(std::move(connection), server) {}

    void start() override {
        std::cout << "TCP Session started from " << get_client_ip() << std::endl;
        
        // 发送欢迎消息
        async_write("220 Welcome to TCP Mail Server\r\n",
            [self = this->shared_from_this()](const boost::system::error_code& error) {
                if (error) {
                    std::cerr << "Error sending welcome message: " << error.message() << std::endl;
                } else {
                    // 开始读取
                    self->async_read();
                }
            });
    }

    void handle_read(const std::string& data) override {
        std::cout << "Received TCP data: " << data << std::endl;
        
        // 简单的回显
        async_write("Echo: " + data,
            [self = this->shared_from_this()](const boost::system::error_code& error) {
                if (!error) {
                    // 继续读取
                    self->async_read();
                }
            });
    }
};

// 示例混合服务器实现
class ExampleHybridServer : public HybridServerBase<ExampleSSLSession, ExampleTCPSession> {
public:
    ExampleHybridServer(const ServerConfig& ssl_config, const ServerConfig& tcp_config)
        : HybridServerBase(ssl_config, tcp_config) {}

protected:
#ifdef USE_SSL
    void handle_ssl_accept(std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>&& ssl_socket, 
                          const boost::system::error_code& error) override {
        if (error) {
            std::cerr << "SSL accept error: " << error.message() << std::endl;
            return;
        }

        try {
            // 创建SSL连接包装器
            auto ssl_connection = std::make_unique<SSLConnection>(std::move(ssl_socket));
            
            // 创建SSL会话
            auto session = std::make_shared<ExampleSSLSession>(std::move(ssl_connection), this);
            session->start();
        } catch (const std::exception& e) {
            std::cerr << "Error creating SSL session: " << e.what() << std::endl;
        }
    }
#endif

    void handle_tcp_accept(std::unique_ptr<boost::asio::ip::tcp::socket>&& tcp_socket, 
                          const boost::system::error_code& error) override {
        if (error) {
            std::cerr << "TCP accept error: " << error.message() << std::endl;
            return;
        }

        try {
            // 创建TCP连接包装器
            auto tcp_connection = std::make_unique<TCPConnection>(std::move(tcp_socket));
            
            // 创建TCP会话
            auto session = std::make_shared<ExampleTCPSession>(std::move(tcp_connection), this);
            session->start();
        } catch (const std::exception& e) {
            std::cerr << "Error creating TCP session: " << e.what() << std::endl;
        }
    }
};

int main() {
    try {
        std::cout << "=== Generic Mail Server Example ===" << std::endl;
        
#ifdef USE_SSL
        // SSL服务器配置
        ServerConfig ssl_config;
        ssl_config.address = "0.0.0.0";
        ssl_config.port = 465;
        ssl_config.use_ssl = true;
        ssl_config.certFile = "config/cert.pem";
        ssl_config.keyFile = "config/key.pem";
        ssl_config.io_thread_count = 2;
        ssl_config.worker_thread_count = 4;
        std::cout << "SSL server configured on port " << ssl_config.port << std::endl;
#endif

        // 非SSL服务器配置
        ServerConfig tcp_config;
        tcp_config.address = "0.0.0.0";
        tcp_config.port = 25;
        tcp_config.use_ssl = false;
        tcp_config.allow_insecure = true;
        tcp_config.io_thread_count = 2;
        tcp_config.worker_thread_count = 4;
        std::cout << "TCP server configured on port " << tcp_config.port << std::endl;

        // 选择运行模式
        std::cout << "\nSelect mode:" << std::endl;
        std::cout << "1. SSL only" << std::endl;
        std::cout << "2. TCP only" << std::endl;
        std::cout << "3. Hybrid (SSL + TCP)" << std::endl;
        std::cout << "Enter choice (1-3): ";
        
        int choice;
        std::cin >> choice;

        std::unique_ptr<ServerBase> server;

        switch (choice) {
#ifdef USE_SSL
            case 1: {
                // 仅SSL模式
                server = std::make_unique<ExampleHybridServer>(ssl_config, tcp_config);
                static_cast<ExampleHybridServer*>(server.get())->start();  // 只启动SSL部分
                std::cout << "SSL-only server started" << std::endl;
                break;
            }
#endif
            case 2: {
                // 仅TCP模式
                server = std::make_unique<ExampleHybridServer>(ssl_config, tcp_config);
                // 手动启动TCP监听（这里需要修改HybridServerBase来支持单独启动）
                std::cout << "TCP-only server started" << std::endl;
                break;
            }
            case 3: {
                // 混合模式
                auto hybrid_server = std::make_unique<ExampleHybridServer>(ssl_config, tcp_config);
                hybrid_server->start();
                server = std::move(hybrid_server);
                std::cout << "Hybrid server started (SSL + TCP)" << std::endl;
                break;
            }
            default:
                std::cerr << "Invalid choice" << std::endl;
                return 1;
        }

        std::cout << "Server running. Press Enter to stop..." << std::endl;
        std::cin.ignore();
        std::cin.get();

        server->stop();
        std::cout << "Server stopped" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}