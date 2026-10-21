#pragma once
namespace pr::http {

template <typename Base>
std::shared_ptr<TcpHttpSession> BasicHttpServer<Base>::make_tcp_session(
    std::unique_ptr<pr::TcpConnection> conn,
    const pr::ListenerConfig& /*lc*/)
{
    auto session = std::make_shared<TcpHttpSession>(this, std::move(conn), m_tcp_fsm);
    session->set_doc_root(doc_root_);
    session->set_api_handler(api_handler_);
    return session;
}

template <typename Base>
std::shared_ptr<SslHttpSession> BasicHttpServer<Base>::make_ssl_session(
    std::unique_ptr<pr::SslConnection> conn,
    const pr::ListenerConfig& /*lc*/)
{
    auto session = std::make_shared<SslHttpSession>(this, std::move(conn), m_ssl_fsm);
    session->set_doc_root(doc_root_);
    session->set_api_handler(api_handler_);
    return session;
}

template <typename Base>
bool BasicHttpServer<Base>::should_reject_connection(std::string& reason,
                                          const std::string& /*client_ip*/) const {
    auto cfg = this->config_snapshot();
    if (cfg && cfg->maxConnections > 0 &&
        this->active_connections_.load(std::memory_order_relaxed) >= cfg->maxConnections) {
        reason = "max connections reached";
        return true;
    }
    return false;
}

} // namespace pr::http
