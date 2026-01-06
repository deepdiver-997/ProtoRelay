#ifndef MAIL_SYSTEM_SERVER_BASE_H
#define MAIL_SYSTEM_SERVER_BASE_H

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <memory>
#include <string>
#include "server_config.h"

#include "mail_system/back/thread_pool/thread_pool_base.h"
#include "mail_system/back/thread_pool/io_thread_pool.h"
#include "mail_system/back/thread_pool/boost_thread_pool.h"

#include "mail_system/back/db/db_pool.h"
#include "mail_system/back/db/db_service.h"
#include "mail_system/back/db/mysql_pool.h"
#include "mail_system/back/db/mysql_service.h"
#include "mail_system/back/entities/mail.h"
// #include "mail_system/back/mailServer/session/session_base.h"

// #include "mail_system/back/mailServer/fsm/client/client_fsm.hpp"

namespace mail_system {

enum class ServerState {
    Stopped,    // 未启动或完全关闭
    Running,    // 正常服务
    Pausing,    // 停止接受新连接，但处理存量请求
    Paused      // 完全暂停（无新连接，无请求处理，随时可以启动服务）
};


class ServerBase {
class SessionBase;
public:
    ServerBase(const ServerConfig& config,
         std::shared_ptr<mail_system::ThreadPoolBase> ioThreadPool = nullptr,
         std::shared_ptr<mail_system::ThreadPoolBase> workerThreadPool = nullptr,
         std::shared_ptr<DBPool> dbPool = nullptr); //allowing sharing pool with other servers
    virtual ~ServerBase();

    ServerBase(const ServerBase&) = delete;
    ServerBase& operator=(const ServerBase&) = delete;
    ServerBase(ServerBase&&) = delete;
    ServerBase& operator=(ServerBase&&) = delete;

    // 启动服务器
    void start();
    // 运行服务器（阻塞直到停止）
    void run();
    // 停止服务器
    void stop(ServerState state = ServerState::Pausing);
    // 是否正在运行
    ServerState get_state() const;
    // 发送异步响应
    void send_async_response(std::weak_ptr<SessionBase> session, const std::string& response);

    void pass_stream(std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket >>&& ssl_socket) {
        this->handle_accept(std::move(ssl_socket), boost::system::error_code());
    }

protected:
    // 创建新会话
    virtual void accept_ssl_connection();
    virtual void accept_tcp_connection();
    // 向后兼容的接口
    virtual void accept_connection();
    virtual void non_ssl_accept_connection();
    // 处理新连接
    virtual void handle_accept(std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket >>&& ssl_socket, const boost::system::error_code& error) = 0;
    // virtual void handle_ssl_connection() = 0;
    virtual void handle_tcp_accept(std::unique_ptr<boost::asio::ip::tcp::socket>&& socket, const boost::system::error_code& error) = 0;
    // 异步连接到指定服务器
    void async_ssl_connect(
        const std::string& host, 
        int port, 
        std::function<void(std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>&&, const boost::system::error_code&)> handler
    );
    // 转发邮件
    void start_forward_email(std::shared_ptr<mail> email);
    // 转发邮件（unique_ptr版本）
    void start_forward_email(mail* email);
    // 加载已知域名
    void load_known_domains(const char* domain_file);
    // 获取连接
    // boost::asio::ssl::stream<boost::asio::ip::tcp::socket>&& get_connection();
public:
    // 获取IO上下文
    std::shared_ptr<boost::asio::io_context> get_io_context();
    // 获取SSL上下文
    boost::asio::ssl::context& get_ssl_context();
    // 获取SSL接受器
    std::shared_ptr<boost::asio::ip::tcp::acceptor> get_ssl_acceptor();
    // 获取TCP接受器
    std::shared_ptr<boost::asio::ip::tcp::acceptor> get_tcp_acceptor();
    // 向后兼容
    std::shared_ptr<boost::asio::ip::tcp::acceptor> get_acceptor();

public:
    std::shared_ptr<mail_system::ThreadPoolBase> m_ioThreadPool;
    std::shared_ptr<mail_system::ThreadPoolBase> m_workerThreadPool;
    std::shared_ptr<DBPool> m_dbPool;
    // std::shared_ptr<ClientFSM> m_client_fsm;
    bool ssl_in_worker;
    const std::string m_domain = "example.com";
    ServerConfig m_config;

protected:
    // 加载SSL证书
    void load_certificates(const std::string& cert_file, const std::string& key_file, const std::string& dh_file = "");

    // 端点配置
    boost::asio::ip::tcp::endpoint m_ssl_endpoint;
    boost::asio::ip::tcp::endpoint m_tcp_endpoint;
    // IO上下文
    std::shared_ptr<boost::asio::io_context> m_ioContext;
    // SSL上下文（仅当启用SSL时初始化）
    boost::asio::ssl::context m_sslContext;
    // 接受器
    std::shared_ptr<boost::asio::ip::tcp::acceptor> m_ssl_acceptor;  // SSL接受器
    std::shared_ptr<boost::asio::ip::tcp::acceptor> m_tcp_acceptor;  // TCP接受器
    std::shared_ptr<boost::asio::ip::tcp::acceptor> m_acceptor;       // 向后兼容
    // 解析器
    std::shared_ptr<boost::asio::ip::tcp::resolver> m_resolver;
    // 工作守卫
    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> m_workGuard;
    // 配置标志
    bool m_enable_ssl;
    bool m_enable_tcp;
    // 是否正在运行
    std::atomic<bool> has_listener_thread;
    // 监听线程
    std::thread m_listenerThread;
    // 服务器状态
    std::atomic<ServerState> m_state;
    // 已知域名
    std::map<std::string, std::string> m_known_domains;     //<domain, ip_address>
};

} // namespace mail_system

#endif // MAIL_SYSTEM_SERVER_BASE_H