#include "mail_system/back/mailServer/imaps_server.h"
#include "mail_system/back/mailServer/connection/ssl_connection.h"
#include "mail_system/back/mailServer/connection/tcp_connection.h"
#include "mail_system/back/common/logger.h"
#include <iostream>
#include <memory>

namespace mail_system {

ImapsServer::ImapsServer(const ServerConfig& config,
     std::shared_ptr<ThreadPoolBase> ioThreadPool,
      std::shared_ptr<ThreadPoolBase> workerThreadPool,
       std::shared_ptr<DBPool> dbPool)
        : ServerBase(config, ioThreadPool, workerThreadPool, dbPool) {
    auto cfg = std::atomic_load(&m_config);

    // IMAP doesn't need PersistentQueue like SMTP (read-only retrieval)
    // but we still need storage provider for reading mail bodies
    // ServerBase already creates m_storageProvider

    // Create FSM instances for TCP and SSL connections
    m_tcp_fsm = std::make_shared<TraditionalImapsFsm<TcpConnection>>(
        m_ioThreadPool, m_workerThreadPool, m_dbPool, m_storageProvider);
    m_ssl_fsm = std::make_shared<TraditionalImapsFsm<SslConnection>>(
        m_ioThreadPool, m_workerThreadPool, m_dbPool, m_storageProvider);

    LOG_IMAP_INFO("IMAP server initialized, SSL fsm={}, TCP fsm={}",
                  m_ssl_fsm ? "ready" : "null",
                  m_tcp_fsm ? "ready" : "null");
}

ImapsServer::~ImapsServer() {
    stop();
}

void ImapsServer::handle_accept(
    std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>&& ssl_socket,
    const boost::system::error_code& error)
{
    using SslSession = ImapsSession<SslConnection>;
    auto ssl_connection = std::make_unique<SslConnection>(std::move(ssl_socket));
    std::unique_ptr<SslSession> session = std::make_unique<SslSession>(
        this, std::move(ssl_connection), m_ssl_fsm);

    if (!error) {
        try {
            LOG_NETWORK_INFO("New IMAPS connection from {}", session->get_client_ip());
            increment_connection_count();
            SslSession::start(std::move(session));
        }
        catch (const std::exception& e) {
            LOG_NETWORK_ERROR("Error starting IMAPS session: {}", e.what());
        }
    } else {
        LOG_NETWORK_ERROR("IMAPS accept error: {}", error.message());
    }
}

void ImapsServer::handle_tcp_accept(
    std::unique_ptr<boost::asio::ip::tcp::socket>&& socket,
    const boost::system::error_code& error)
{
    using TcpSession = ImapsSession<TcpConnection>;
    std::unique_ptr<TcpConnection> tcp_connection = std::make_unique<TcpConnection>(std::move(socket));
    std::unique_ptr<TcpSession> session = std::make_unique<TcpSession>(
        this, std::move(tcp_connection), m_tcp_fsm);

    if (!error) {
        try {
            LOG_NETWORK_INFO("New IMAP connection from {}", session->get_client_ip());
            increment_connection_count();
            TcpSession::start(std::move(session));
        }
        catch (const std::exception& e) {
            LOG_NETWORK_ERROR("Error starting IMAP session: {}", e.what());
        }
    } else {
        LOG_NETWORK_ERROR("IMAP accept error: {}", error.message());
    }
}

bool ImapsServer::should_reject_connection(std::string& reason) const {
    auto cfg = std::atomic_load(&m_config);
    if (cfg->maxConnections > 0 &&
        active_connections_.load(std::memory_order_relaxed) >= cfg->maxConnections) {
        reason = "max connections reached";
        return true;
    }
    return false;
}

} // namespace mail_system
