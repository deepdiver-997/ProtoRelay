// QUIC listener: binds one UDP endpoint, delivers established connections as
// IMuxConnection. The TLS identity is configured separately (configure_tls,
// IServerTransport discipline) and may be omitted — handshakes then fail
// naturally for lack of a certificate.
#include "quic_transport_impl.h"

#include <arpa/inet.h>
#include <netinet/in.h>

#include <cassert>
#include <stdexcept>

namespace pr::net::quic {

namespace {
uint16_t sa_port(const struct sockaddr* sa) {
    if (sa->sa_family == AF_INET)
        return ntohs(reinterpret_cast<const sockaddr_in*>(sa)->sin_port);
    return ntohs(reinterpret_cast<const sockaddr_in6*>(sa)->sin6_port);
}
} // namespace

class QuicServerTransport final : public IQuicServerTransport, private QuicTransportBase {
public:
    QuicServerTransport(QuicServerConfig config, const std::shared_ptr<IExecutor>& executor)
        : config_(std::move(config)) {
        auto loop = std::dynamic_pointer_cast<NativeLoop>(executor);
        if (!loop)
            throw std::invalid_argument("quic server requires a NativeLoop executor");

        QuicEngineCore::Params params;
        params.loop = loop;
        params.engine_flags = LSENG_SERVER;
        params.lookup_cert = [](void* ctx, const struct sockaddr*, const char*) -> ssl_ctx_st* {
            return static_cast<QuicServerTransport*>(ctx)->ssl_ctx_;
        };
        params.cert_lu_ctx = this;
        params.get_ssl_ctx = [](void* peer_ctx, const struct sockaddr*) -> ssl_ctx_st* {
            return static_cast<QuicServerTransport*>(peer_ctx)->ssl_ctx_;
        };
        params.packet_peer_ctx = this;
        core_ = std::make_shared<QuicEngineCore>(std::move(params));
        core_->attach_transport(this);

        // Construction binds (IServerTransport discipline); port 0 is
        // replaced by the actually bound port.
        std::string err;
        int family = config_.address.find(':') != std::string::npos ? AF_INET6 : AF_INET;
        bool bound = false;
        run_on_loop(*loop, [&] { bound = core_->open_socket(family, config_.address, config_.port, err); });
        if (!bound) throw std::runtime_error("quic server: " + err);
        config_.port = sa_port(core_->local_sa(family));
    }

    ~QuicServerTransport() override {
        core_->attach_transport(nullptr);
        // Engine destruction closes every live connection; undelivered
        // wrappers die with this object, delivered ones detach via their own
        // destructor or the null-ctx guard.
        QuicEngineCore::destroy(std::move(core_));
    }

    const QuicServerConfig& config() const override { return config_; }

    void configure_tls(const TlsConfig& tls) override {
        assert(!started_); // at most once, before start()
        ssl_ctx_ = make_server_ssl_ctx(tls);
        if (!ssl_ctx_)
            throw std::runtime_error("quic server: failed to load certificate/key from " +
                                     tls.certificate_file);
    }

    void start(MuxAcceptHandler handler) override {
        assert(!started_);
        started_ = true;
        handler_ = std::move(handler);
        core_->kick(); // begins the advisory-tick cycle
    }

    void stop() override {
        if (stopped_) return;
        stopped_ = true;
        // Listener only: no more datagrams are read, so no new handshakes can
        // complete. Delivered connections stay in the engine (which lives
        // until this transport is destroyed).
        auto core = core_;
        run_on_loop(core->loop(), [core] { core->close_sockets(); });
    }

private:
    // QuicTransportBase (loop thread)
    // On the IETF server the first flight is handled by an internal mini
    // connection; on_new_conn fires when it is promoted to a full connection
    // — at that point the handshake is already complete (the reference
    // project's proxy delivers its sessions on this exact callback).
    void on_new_conn(lsquic_conn_t* conn) override {
        auto wrapper = std::make_unique<QuicMuxConnection>(core_, conn, /*is_client=*/false);
        if (!handler_) return; // not started (or stopped): conn is discarded
        auto box = std::make_shared<std::unique_ptr<IMuxConnection>>(std::move(wrapper));
        auto loop = core_->loop_ptr();
        auto handler = handler_;
        loop->post([loop, handler = std::move(handler), box]() mutable {
            handler(std::move(*box));
        });
    }

    void on_hsk_done(lsquic_conn_t*, lsquic_hsk_status) override {
        // Server-side handshakes complete inside the mini connection; this
        // callback does not fire on the promoted full connection. The client
        // transport delivers on it instead.
    }

    void on_conn_closed(lsquic_conn_t*) override {
        // Delivered wrappers are owned by the application and detach via
        // their own lsquic callback; nothing to clean up here.
    }

    QuicServerConfig config_;
    std::shared_ptr<QuicEngineCore> core_;
    ssl_ctx_st* ssl_ctx_ = nullptr; // owned; BoringSSL (prefixed)
    MuxAcceptHandler handler_;
    bool started_ = false;
    bool stopped_ = false;
};

} // namespace pr::net::quic

namespace pr::net::quic {

std::unique_ptr<IQuicServerTransport> make_quic_server_transport(
    const QuicServerConfig& config, std::shared_ptr<IExecutor> executor) {
    return std::make_unique<QuicServerTransport>(config, std::move(executor));
}

} // namespace pr::net::quic
