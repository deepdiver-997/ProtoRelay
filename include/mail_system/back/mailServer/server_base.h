#ifndef MAIL_SYSTEM_SERVER_BASE_H
#define MAIL_SYSTEM_SERVER_BASE_H

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include "server_config.h"
#include "intrusion_detector.h"

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
#include "mail_system/back/common/lru_cache.h"
#include "mail_system/back/storage/i_storage_provider.h"
#include "mail_system/back/mailServer/metrics_server.h"

namespace mail_system {

enum class ServerState { Stopped, Running, Pausing, Paused };

class ServerBase {
class SessionBase;
public:
    ServerBase(const ServerConfig& config,
         std::shared_ptr<mail_system::ThreadPoolBase> ioThreadPool = nullptr,
         std::shared_ptr<mail_system::ThreadPoolBase> workerThreadPool = nullptr,
         std::shared_ptr<DBPool> dbPool = nullptr);
    virtual ~ServerBase();

    ServerBase(const ServerBase&) = delete;
    ServerBase& operator=(const ServerBase&) = delete;
    ServerBase(ServerBase&&) = delete;
    ServerBase& operator=(ServerBase&&) = delete;

    void start();
    void run();
    void stop(ServerState state = ServerState::Pausing);
    ServerState get_state() const;

    void pass_stream(std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>&& ssl_socket,
                     const ListenerConfig& lc) {
        this->handle_accept(std::move(ssl_socket), boost::system::error_code(), lc);
    }

    virtual void handoff_starttls_socket(std::unique_ptr<boost::asio::ip::tcp::socket>&& socket);

    const ListenerConfig* get_listener_config(uint16_t port) const;

protected:
    // 启动所有 listener 的异步 accept
    virtual void start_all_tcp_acceptors();
    virtual void start_all_ssl_acceptors();
    void do_tcp_accept(std::shared_ptr<boost::asio::ip::tcp::acceptor> acceptor, const ListenerConfig& lc);
    void do_ssl_accept(std::shared_ptr<boost::asio::ip::tcp::acceptor> acceptor, const ListenerConfig& lc);

    // 处理 TLS acceptor 来的新连接
    virtual void handle_accept(std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>&& ssl_socket,
                               const boost::system::error_code& error, ListenerConfig lc) = 0;
    // 处理 TCP acceptor 来的新连接
    virtual void handle_tcp_accept(std::unique_ptr<boost::asio::ip::tcp::socket>&& socket,
                                   const boost::system::error_code& error, ListenerConfig lc) = 0;

public:
    std::shared_ptr<boost::asio::io_context> get_io_context();
    boost::asio::ssl::context& get_ssl_context();

public:
    std::shared_ptr<mail_system::ThreadPoolBase> m_ioThreadPool;
    std::shared_ptr<mail_system::ThreadPoolBase> m_workerThreadPool;
    std::shared_ptr<DBPool> m_dbPool;
    std::shared_ptr<persist_storage::PersistentQueue> m_persistentQueue;
    std::shared_ptr<outbound::SmtpOutboundClient> m_outboundClient;
    std::shared_ptr<std::atomic<bool>> m_outboundInterruptFlag;
    std::shared_ptr<storage::IStorageProvider> m_storageProvider;

    void set_mailbox_cache(std::shared_ptr<IMailboxCache> cache) { m_mailboxCache = cache; }
    std::shared_ptr<IMailboxCache> get_mailbox_cache() const { return m_mailboxCache; }

    bool ssl_in_worker;
    std::shared_ptr<IMailboxCache> m_mailboxCache;
    std::string m_domain;
    std::string m_configFilePath;
    std::shared_ptr<ServerConfig> m_config;

    bool reload_config(const std::string& json_file);

    IntrusionDetector m_intrusionDetector;
    void record_session_end(const std::string& ip, bool authenticated) {
        m_intrusionDetector.record_session(ip, authenticated);
    }

    // IP 封禁检查（在 accept 层调用）
    bool is_ip_banned(const std::string& ip) const {
        auto cfg = std::atomic_load(&m_config);
        if (!cfg->intrusion_detection_enabled || cfg->intrusion_ban_threshold <= 0) return false;
        return m_intrusionDetector.is_banned(ip);
    }

    std::atomic<size_t> active_connections_{0};
    void increment_connection_count() { active_connections_.fetch_add(1, std::memory_order_relaxed); }
    void decrement_connection_count() { active_connections_.fetch_sub(1, std::memory_order_relaxed); }

    std::atomic<size_t> connections_total_{0};
    std::atomic<size_t> connections_rejected_total_{0};
    std::atomic<size_t> mails_accepted_total_{0};
    void increment_connections_total() { connections_total_.fetch_add(1, std::memory_order_relaxed); }
    void increment_connections_rejected() { connections_rejected_total_.fetch_add(1, std::memory_order_relaxed); }
    void increment_mails_accepted() { mails_accepted_total_.fetch_add(1, std::memory_order_relaxed); }

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
        add_gauge("protorelay_active_connections", "Current active connections",
                  active_connections_.load(std::memory_order_relaxed));
        add_counter("protorelay_connections_total", "Total connections accepted",
                    connections_total_.load(std::memory_order_relaxed));
        add_counter("protorelay_connections_rejected_total", "Total connections rejected",
                    connections_rejected_total_.load(std::memory_order_relaxed));
        add_counter("protorelay_mails_accepted_total", "Total mails accepted",
                    mails_accepted_total_.load(std::memory_order_relaxed));
        if (m_persistentQueue)
            add_gauge("protorelay_inflight_mails", "Current inflight mails",
                      m_persistentQueue->inflight_count());
        if (m_dbPool) {
            add_gauge("protorelay_db_pool_size", "DB pool size", m_dbPool->get_pool_size());
            add_gauge("protorelay_db_available", "DB idle connections", m_dbPool->get_available_connections());
            add_gauge("protorelay_db_active", "DB active connections", m_dbPool->get_active_connections());
            add_gauge("protorelay_db_pool_max", "DB pool max", m_dbPool->get_max_pool_size());
        }
        return out;
    }

protected:
    void load_certificates(const std::string& cert_file, const std::string& key_file, const std::string& dh_file = "");

    std::shared_ptr<boost::asio::io_context> m_ioContext;
    boost::asio::ssl::context m_sslContext;

    // 多监听器
    std::vector<std::shared_ptr<boost::asio::ip::tcp::acceptor>> m_tcp_acceptors;
    std::vector<std::shared_ptr<boost::asio::ip::tcp::acceptor>> m_ssl_acceptors;
    std::unordered_map<uint16_t, ListenerConfig> m_listener_configs;

    std::shared_ptr<boost::asio::ip::tcp::resolver> m_resolver;
    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> m_workGuard;

    virtual bool should_reject_connection(std::string& reason, const std::string& client_ip = "") const = 0;

    void start_metrics_server();
    void stop_metrics_server();
    std::unique_ptr<MetricsServer> metrics_server_;

    std::atomic<bool> has_listener_thread;
    std::thread m_listenerThread;
    std::atomic<ServerState> m_state;
    std::map<std::string, std::string> m_known_domains;
};

} // namespace mail_system
#endif
