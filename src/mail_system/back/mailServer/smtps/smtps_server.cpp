#include "mail_system/back/mailServer/smtps_server.h"
#include "mail_system/back/mailServer/connection/ssl_connection.h"
#include "mail_system/back/mailServer/connection/tcp_connection.h"
#include "mail_system/back/outbound/cares_dns_resolver.h"
#include "mail_system/back/common/logger.h"
#include <iostream>
#include <memory>

namespace mail_system {

SmtpsServer::SmtpsServer(const ServerConfig& config,
     std::shared_ptr<ThreadPoolBase> ioThreadPool,
      std::shared_ptr<ThreadPoolBase> wokerThreadPool,
       std::shared_ptr<DBPool> dbPool)
        : ServerBase(config, ioThreadPool, wokerThreadPool, dbPool) {
    auto cfg = std::atomic_load(&m_config);

    // SMTP 专用：创建持久化队列和出站投递客户端
    // 这些是 SMTP 私有的，IMAP 不需要
    if (m_dbPool) {
        if (!m_persistentQueue) {
            m_persistentQueue = std::make_shared<persist_storage::PersistentQueue>(
                m_dbPool,
                m_workerThreadPool,
                m_storageProvider);
            m_persistentQueue->set_local_domain(m_domain);
            persist_storage::PersistentQueuePressureConfig pressure_config;
            pressure_config.max_inflight_mails = cfg->persist_max_inflight_mails;
            pressure_config.min_available_memory_mb = cfg->persist_min_available_memory_mb;
            pressure_config.min_db_available_connections = cfg->persist_min_db_available_connections;
            m_persistentQueue->set_pressure_config(pressure_config);
            LOG_SERVER_INFO("PersistentQueue created for SMTP server");
        }

        if (!m_outboundInterruptFlag) {
            m_outboundInterruptFlag = std::make_shared<std::atomic<bool>>(true);
        }

        if (!m_outboundClient) {
            outbound::OutboundIdentityConfig outbound_identity;
            outbound_identity.helo_domain = cfg->outbound_helo_domain;
            outbound_identity.mail_from_domain = cfg->outbound_mail_from_domain;
            outbound_identity.rewrite_header_from = cfg->outbound_rewrite_header_from;
            outbound_identity.dkim_enabled = cfg->outbound_dkim_enabled;
            outbound_identity.dkim_selector = cfg->outbound_dkim_selector;
            outbound_identity.dkim_domain = cfg->outbound_dkim_domain;
            outbound_identity.dkim_private_key_file = cfg->outbound_dkim_private_key_file;

            outbound::OutboundPollingConfig outbound_polling;
            outbound_polling.busy_sleep_ms = static_cast<int>(cfg->outbound_poll_busy_sleep_ms);
            outbound_polling.backoff_base_ms = static_cast<int>(cfg->outbound_poll_backoff_base_ms);
            outbound_polling.backoff_max_ms = static_cast<int>(cfg->outbound_poll_backoff_max_ms);
            outbound_polling.backoff_shift_cap = static_cast<std::size_t>(cfg->outbound_poll_backoff_shift_cap);

            m_outboundClient = std::make_shared<outbound::SmtpOutboundClient>(
                m_dbPool,
                m_ioThreadPool,
                m_workerThreadPool,
                std::make_shared<outbound::CaresDnsResolver>(),
                m_outboundInterruptFlag,
                std::move(outbound_identity),
                outbound_polling,
                m_domain,
                cfg->outbound_ports,
                static_cast<int>(cfg->outbound_max_attempts)
            );
            m_persistentQueue->set_outbound_client(m_outboundClient);
            m_outboundClient->start();
            LOG_SERVER_INFO("Outbound client created and started for SMTP server");
        }
    } else {
        LOG_SERVER_WARN("No database pool — SMTP outbound delivery disabled");
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

            increment_connection_count();
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

            increment_connection_count();
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

bool SmtpsServer::should_reject_connection(std::string& reason) const {
    auto cfg = std::atomic_load(&m_config);
    if (cfg->maxConnections > 0 &&
        active_connections_.load(std::memory_order_relaxed) >= cfg->maxConnections) {
        reason = "max connections reached";
        return true;
    }

    if (m_persistentQueue &&
        cfg->persist_max_inflight_mails > 0 &&
        m_persistentQueue->inflight_count() >= cfg->persist_max_inflight_mails) {
        reason = "persist inflight limit reached";
        return true;
    }

    return false;
}

} // namespace mail_system
