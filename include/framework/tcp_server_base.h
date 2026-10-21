#ifndef PR_FRAMEWORK_TCP_SERVER_BASE_H
#define PR_FRAMEWORK_TCP_SERVER_BASE_H
#include "framework/server_base.h"
#include "framework/log/log_macros.h"
#include "framework/connection/tcp_connection.h"
#include "framework/connection/ssl_connection.h"
#include "framework/net/server_transport.h"
#include <csignal>
#include <stdexcept>

namespace pr {
// Protocol-facing server: owns admission policy and session factories. A runtime
// provides listening and TLS; it need not expose contexts, guards or acceptors.
template <typename TcpSession, typename SslSession, typename Base = ServerBase>
class TcpServerBase : public Base {
public:
    template <typename... Resources>
    TcpServerBase(const typename Base::Config& config,
                  std::shared_ptr<ThreadPoolBase> io,
                  std::shared_ptr<ThreadPoolBase> workers, Resources&&... resources)
        : Base(config, std::move(io), std::move(workers), std::forward<Resources>(resources)...) {
        auto provider = std::dynamic_pointer_cast<net::IServerTransportProvider>(this->m_ioThreadPool);
        if (!provider) {
            if (!config.listeners.empty()) throw std::invalid_argument("IO runtime must provide server transports");
            return;
        }
        transport_ = provider->make_server_transport({config.address, config.listeners,
            {config.certFile, config.keyFile, config.dhFile, config.enable_tls1_3}});
        if (!transport_) throw std::runtime_error("IO runtime returned an empty server transport");
        for (const auto& listener : transport_->listeners()) {
            m_listener_configs[listener.port] = listener;
            LOG_SERVER_INFO("{} listener on {}:{}", listener_type_to_string(listener.type), config.address, listener.port);
        }
    }
    ~TcpServerBase() override { stop(); }
    void start() override;
    virtual void handoff_starttls(std::unique_ptr<IConnection> connection, std::string trace = {});
    void load_certificates(const std::string& certificate, const std::string& key,
                           const std::string& dh = "", bool tls1_3 = true) {
        if (!transport_) throw std::logic_error("server has no network transport");
        transport_->configure_tls({certificate, key, dh, tls1_3});
    }
    std::unordered_map<uint16_t, ListenerConfig> m_listener_configs;
protected:
    void stop(ServerState state = ServerState::Pausing) override;
    virtual std::shared_ptr<TcpSession> make_tcp_session(
        std::unique_ptr<TcpConnection> connection, const ListenerConfig& listener) = 0;
    virtual std::shared_ptr<SslSession> make_ssl_session(
        std::unique_ptr<SslConnection> connection, const ListenerConfig& listener) = 0;
private:
    void accepted(std::unique_ptr<IConnection> connection, const ListenerConfig& listener);
    std::unique_ptr<net::IServerTransport> transport_;
};

template <typename TcpSession, typename SslSession, typename Base>
void TcpServerBase<TcpSession, SslSession, Base>::start() {
    if (this->m_state.load() != ServerState::Paused) return;
    // OS zero-copy writes can raise SIGPIPE when a peer disconnects.
    std::signal(SIGPIPE, SIG_IGN);
    this->m_state.store(ServerState::Running);
    try {
        this->start_metrics_server();
        if (transport_) transport_->start([this](std::unique_ptr<IConnection> connection, const ListenerConfig& listener) {
            accepted(std::move(connection), listener);
        });
    } catch (...) {
        stop();
        throw;
    }
}

template <typename TcpSession, typename SslSession, typename Base>
void TcpServerBase<TcpSession, SslSession, Base>::stop(ServerState state) {
    // stop() quiesces accept callbacks before any derived state is destroyed.
    if (transport_) transport_->stop();
    Base::stop(state);
}

template <typename TcpSession, typename SslSession, typename Base>
void TcpServerBase<TcpSession, SslSession, Base>::accepted(
    std::unique_ptr<IConnection> connection, const ListenerConfig& listener) {
    if (!connection) return;
    if (this->get_state() != ServerState::Running) { connection->close(); return; }
    const auto ip = connection->get_remote_ip();
    std::string reason;
    if (this->is_ip_banned(ip) || this->should_reject_connection(reason, ip)) {
        this->increment_connections_rejected();
        LOG_NETWORK_WARN("Rejecting connection from {}: {}", ip, reason);
        connection->close();
        return;
    }
    this->increment_connections_total();
    if (listener.type == ListenerType::SSL) {
        auto session = make_ssl_session(std::make_unique<SslConnection>(std::move(connection)), listener);
        if (session) {
            this->increment_connection_count();
            session->set_tracks_connection_count(true);
            SslSession::start(session);
        }
    } else {
        auto session = make_tcp_session(std::make_unique<TcpConnection>(std::move(connection)), listener);
        if (session) {
            this->increment_connection_count();
            session->set_tracks_connection_count(true);
            TcpSession::start(session);
        }
    }
}

template <typename TcpSession, typename SslSession, typename Base>
void TcpServerBase<TcpSession, SslSession, Base>::handoff_starttls(
    std::unique_ptr<IConnection> connection, std::string trace) {
    if (!connection) return;
    if (!transport_) throw std::logic_error("server has no network transport");
    ListenerConfig listener;
    if (auto found = m_listener_configs.find(connection->get_local_port()); found != m_listener_configs.end())
        listener = found->second;
    // Preserve listener authentication policy and trace across the handoff.
    auto tls = transport_->upgrade_tls(take_connection_transport(std::move(connection)));
    if (!tls) throw std::runtime_error("network backend returned an empty TLS connection");
    auto session = make_ssl_session(std::make_unique<SslConnection>(std::move(tls)), listener);
    if (session) {
        this->increment_connection_count();
        session->set_tracks_connection_count(true);
        if (!trace.empty()) session->set_trace_buffer(std::move(trace));
        SslSession::start_after_starttls(session);
    }
}
} // namespace pr
#endif
