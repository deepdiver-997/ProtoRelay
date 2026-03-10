#include "mail_system/back/mailServer/smtps_server.h"
#include "mail_system/back/mailServer/connection/ssl_connection.h"
#include "mail_system/back/mailServer/connection/tcp_connection.h"
#include "mail_system/back/common/logger.h"
#include <iostream>
#include <memory>

namespace mail_system {

SmtpsServer::SmtpsServer(const ServerConfig& config,
     std::shared_ptr<ThreadPoolBase> ioThreadPool,
      std::shared_ptr<ThreadPoolBase> wokerThreadPool,
       std::shared_ptr<DBPool> dbPool)
        : ServerBase(config, ioThreadPool, wokerThreadPool, dbPool) {
    // ServerBase may already create and wire PersistentQueue with outbound client.
    // Avoid overriding it here, otherwise outbox enqueue/notify wiring can be lost.
    if (!m_persistentQueue && m_dbPool) {
        m_persistentQueue = std::make_shared<persist_storage::PersistentQueue>(m_dbPool, m_workerThreadPool);
        m_persistentQueue->set_local_domain(m_domain);
        if (m_outboundClient) {
            m_persistentQueue->set_outbound_client(m_outboundClient);
        }
    }
    m_tcp_fsm = std::make_shared<TraditionalSmtpsFsm<TcpConnection>>(m_ioThreadPool, m_workerThreadPool, m_persistentQueue, m_dbPool);
    m_ssl_fsm = std::make_shared<TraditionalSmtpsFsm<SslConnection>>(m_ioThreadPool, m_workerThreadPool, m_persistentQueue, m_dbPool);
}

SmtpsServer::~SmtpsServer() {
    // 确保服务器停止
    stop();
}

void SmtpsServer::handle_accept(std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket> >&& ssl_socket, const boost::system::error_code& error) {
    // 创建会话
    using SslSession = SmtpsSession<mail_system::SslConnection>;
    auto ssl_connection = std::make_unique<SslConnection>(std::move(ssl_socket));
    std::unique_ptr<SslSession> session = std::make_unique<SslSession>(this, std::move(ssl_connection), m_ssl_fsm);
    if (!error) {
        try {
            LOG_NETWORK_INFO("New SMTPS connection from {}", session->get_client_ip());

            // 启动会话 - 直接在当前线程调用以避免unique_ptr复制问题
            SslSession::start(std::move(session));
        }
        catch (const std::exception& e) {
            LOG_NETWORK_ERROR("Error starting SMTPS session: {}", e.what());
        }
    }
    else {
        LOG_NETWORK_ERROR("Accept error: {}", error.message());
    }
}

void SmtpsServer::handle_tcp_accept(std::unique_ptr<boost::asio::ip::tcp::socket>&& socket, const boost::system::error_code& error) {
    // 创建会话
    using TcpSession = SmtpsSession<mail_system::TcpConnection>;
    std::unique_ptr<TcpConnection> tcp_connection = std::make_unique<TcpConnection>(std::move(socket));
    std::unique_ptr<TcpSession> session = std::make_unique<TcpSession>(this, std::move(tcp_connection), m_tcp_fsm);
    if (!error) {
        try {
            LOG_NETWORK_INFO("New SMTP connection from {}", session->get_client_ip());

            // 启动会话 - 直接在当前线程调用以避免unique_ptr复制问题
            TcpSession::start(std::move(session));
        }
        catch (const std::exception& e) {
            LOG_NETWORK_ERROR("Error starting SMTP session: {}", e.what());
        }
    }
    else {
        LOG_NETWORK_ERROR("Accept error: {}", error.message());
    }
}

void SmtpsServer::handoff_starttls_socket(std::unique_ptr<boost::asio::ip::tcp::socket>&& socket) {
    using SslSession = SmtpsSession<mail_system::SslConnection>;
    if (!socket) {
        return;
    }

    try {
        auto ssl_stream = std::make_unique<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>(
            std::move(*socket),
            get_ssl_context());
        auto ssl_connection = std::make_unique<SslConnection>(std::move(ssl_stream));
        auto session = std::make_unique<SslSession>(this, std::move(ssl_connection), m_ssl_fsm);

        LOG_NETWORK_INFO("STARTTLS upgraded, continue SMTP on TLS from {}", session->get_client_ip());
        SslSession::start_after_starttls(std::move(session));
    } catch (const std::exception& e) {
        LOG_NETWORK_ERROR("Error handing off STARTTLS socket: {}", e.what());
    }
}

} // namespace mail_system
