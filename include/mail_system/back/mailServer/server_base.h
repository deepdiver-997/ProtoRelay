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
#include "mail_system/back/persist_storage/persistent_queue.h"
#include "mail_system/back/outbound/smtp_outbound_client.h"
#include "mail_system/back/storage/i_storage_provider.h"
#include "mail_system/back/mailServer/metrics_server.h"
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

    void pass_stream(std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket >>&& ssl_socket) {
        this->handle_accept(std::move(ssl_socket), boost::system::error_code());
    }

    // Upgrade an accepted plain TCP socket to TLS and continue SMTP session flow.
    virtual void handoff_starttls_socket(std::unique_ptr<boost::asio::ip::tcp::socket>&& socket);

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
    std::shared_ptr<persist_storage::PersistentQueue> m_persistentQueue;
    std::shared_ptr<outbound::SmtpOutboundClient> m_outboundClient;
    std::shared_ptr<std::atomic<bool>> m_outboundInterruptFlag;
    std::shared_ptr<storage::IStorageProvider> m_storageProvider;
    // std::shared_ptr<ClientFSM> m_client_fsm;
    bool ssl_in_worker;
    std::string m_domain;
    ServerConfig m_config;

    // 连接负载门控
    std::atomic<size_t> active_connections_{0};

    void increment_connection_count() {
        active_connections_.fetch_add(1, std::memory_order_relaxed);
    }

    void decrement_connection_count() {
        active_connections_.fetch_sub(1, std::memory_order_relaxed);
    }

    // Metrics 计数器
    std::atomic<size_t> connections_total_{0};
    std::atomic<size_t> connections_rejected_total_{0};
    std::atomic<size_t> mails_accepted_total_{0};

    void increment_connections_total() {
        connections_total_.fetch_add(1, std::memory_order_relaxed);
    }
    void increment_connections_rejected() {
        connections_rejected_total_.fetch_add(1, std::memory_order_relaxed);
    }
    void increment_mails_accepted() {
        mails_accepted_total_.fetch_add(1, std::memory_order_relaxed);
    }

    // Prometheus 格式 metrics 输出
    std::string build_metrics_response() const {
        std::string out;
        out.reserve(1024);

        auto add_gauge = [&](const char* name, const char* help, size_t val) {
            out.append("# HELP ").append(name).append(" ").append(help).append("\n");
            out.append("# TYPE ").append(name).append(" gauge\n");
            out.append(name).append(" ").append(std::to_string(val)).append("\n");
        };

        auto add_counter = [&](const char* name, const char* help, size_t val) {
            out.append("# HELP ").append(name).append(" ").append(help).append("\n");
            out.append("# TYPE ").append(name).append(" counter\n");
            out.append(name).append(" ").append(std::to_string(val)).append("\n");
        };

        add_gauge("protorelay_active_connections",
                  "Current active connections",
                  active_connections_.load(std::memory_order_relaxed));

        add_counter("protorelay_connections_total",
                    "Total connections accepted since start",
                    connections_total_.load(std::memory_order_relaxed));

        add_counter("protorelay_connections_rejected_total",
                    "Total connections rejected since start",
                    connections_rejected_total_.load(std::memory_order_relaxed));

        add_counter("protorelay_mails_accepted_total",
                    "Total mails accepted (250 OK sent) since start",
                    mails_accepted_total_.load(std::memory_order_relaxed));

        if (m_persistentQueue) {
            add_gauge("protorelay_inflight_mails",
                      "Current inflight mails in persistence pipeline",
                      m_persistentQueue->inflight_count());
        }

        return out;
    }

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
    // 判断是否应该拒绝新连接（纯虚函数，由子类实现具体负载检查逻辑）
    virtual bool should_reject_connection(std::string& reason) const = 0;

    // Metrics HTTP 服务
    void start_metrics_server();
    void stop_metrics_server();
    std::unique_ptr<MetricsServer> metrics_server_;

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