#include "web_server/h2/h2_server.h"
#include "framework/db/db_pool.h"
#include "mail_system/back/common/logger.h"
#include <memory>

namespace web_server {
namespace h2 {

H2Server::H2Server(const mail_system::ServerConfig& config,
                   std::string doc_root,
                   std::shared_ptr<pr::ThreadPoolBase> ioThreadPool,
                   std::shared_ptr<pr::ThreadPoolBase> workerThreadPool,
                   std::shared_ptr<mail_system::router::IShardRouter> shardRouter)
    : pr::TcpServerBase<TcpH2Session, SslH2Session>(
          config, ioThreadPool, workerThreadPool, shardRouter)
    , doc_root_(std::move(doc_root))
{
    LOG_SERVER_INFO("H2Server (HTTP/2 multi-stream) ready, doc_root={}", doc_root_);
}

std::shared_ptr<TcpH2Session> H2Server::make_tcp_session(
    std::unique_ptr<pr::TcpConnection> conn,
    const mail_system::ListenerConfig& /*lc*/)
{
    auto s = std::make_shared<TcpH2Session>(this, std::move(conn));
    s->set_doc_root(doc_root_);
    return s;
}

std::shared_ptr<SslH2Session> H2Server::make_ssl_session(
    std::unique_ptr<pr::SslConnection> conn,
    const mail_system::ListenerConfig& /*lc*/)
{
    auto s = std::make_shared<SslH2Session>(this, std::move(conn));
    s->set_doc_root(doc_root_);
    return s;
}

bool H2Server::should_reject_connection(std::string& reason,
                                        const std::string& /*client_ip*/) const {
    auto cfg = std::atomic_load(&m_config);
    if (cfg && cfg->maxConnections > 0 &&
        active_connections_.load(std::memory_order_relaxed) >= cfg->maxConnections) {
        reason = "max connections reached";
        return true;
    }
    return false;
}

} // namespace h2
} // namespace web_server