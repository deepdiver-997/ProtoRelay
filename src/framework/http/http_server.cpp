#include "framework/http/http_server.h"
#include "framework/db/db_pool.h"
#include "mail_system/back/common/logger.h"
#include <memory>

namespace pr::http {

HttpServer::HttpServer(const mail_system::ServerConfig& config,
                       std::string doc_root,
                       std::shared_ptr<pr::ThreadPoolBase> ioThreadPool,
                       std::shared_ptr<pr::ThreadPoolBase> workerThreadPool,
                       std::shared_ptr<mail_system::router::IShardRouter> shardRouter)
    : pr::TcpServerBase<TcpHttpSession, SslHttpSession>(
          config, ioThreadPool, workerThreadPool, shardRouter)
    , doc_root_(std::move(doc_root))
{
    m_tcp_fsm = std::make_shared<HttpFsm<pr::TcpConnection>>();
    m_ssl_fsm = std::make_shared<HttpFsm<pr::SslConnection>>();
    LOG_SERVER_INFO("HttpServer ready, doc_root={}", doc_root_);
}

std::shared_ptr<TcpHttpSession> HttpServer::make_tcp_session(
    std::unique_ptr<pr::TcpConnection> conn,
    const mail_system::ListenerConfig& /*lc*/)
{
    auto session = std::make_shared<TcpHttpSession>(this, std::move(conn), m_tcp_fsm);
    session->set_doc_root(doc_root_);
    session->set_api_handler(api_handler_);
    return session;
}

std::shared_ptr<SslHttpSession> HttpServer::make_ssl_session(
    std::unique_ptr<pr::SslConnection> conn,
    const mail_system::ListenerConfig& /*lc*/)
{
    auto session = std::make_shared<SslHttpSession>(this, std::move(conn), m_ssl_fsm);
    session->set_doc_root(doc_root_);
    session->set_api_handler(api_handler_);
    return session;
}

bool HttpServer::should_reject_connection(std::string& reason,
                                          const std::string& /*client_ip*/) const {
    auto cfg = std::atomic_load(&m_config);
    if (cfg && cfg->maxConnections > 0 &&
        active_connections_.load(std::memory_order_relaxed) >= cfg->maxConnections) {
        reason = "max connections reached";
        return true;
    }
    return false;
}

} // namespace pr::http