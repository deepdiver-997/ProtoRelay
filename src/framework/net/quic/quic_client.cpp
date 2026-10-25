// QUIC client dialer: one engine (created lazily on the first connect), one
// UDP socket per family (ephemeral port), mandatory certificate and hostname
// verification through lsquic's ea_verify_cert.
#include "quic_transport_impl.h"
#include "framework/net/tls_error.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/x509.h>

#include <cassert>
#include <stdexcept>
#include <unordered_map>

namespace pr::net::quic {

namespace {
bool is_ip_literal(const std::string& host) {
    in_addr v4{};
    in6_addr v6{};
    return inet_pton(AF_INET, host.c_str(), &v4) == 1 ||
           inet_pton(AF_INET6, host.c_str(), &v6) == 1;
}

bool resolve_udp(const std::string& host, uint16_t port, sockaddr_storage& out, int& family) {
    in_addr v4{};
    in6_addr v6{};
    if (inet_pton(AF_INET, host.c_str(), &v4) == 1) {
        auto* sin = reinterpret_cast<sockaddr_in*>(&out);
        sin->sin_family = AF_INET;
        sin->sin_port = htons(port);
        sin->sin_addr = v4;
        family = AF_INET;
        return true;
    }
    if (inet_pton(AF_INET6, host.c_str(), &v6) == 1) {
        auto* sin6 = reinterpret_cast<sockaddr_in6*>(&out);
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons(port);
        sin6->sin6_addr = v6;
        family = AF_INET6;
        return true;
    }
    addrinfo hints{};
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result)
        return false;
    // A short blocking resolve on the loop thread — same tradeoff as the
    // native TCP client's connect path (numeric hosts skip it entirely).
    out = *reinterpret_cast<sockaddr_storage*>(result->ai_addr);
    family = result->ai_family;
    if (family == AF_INET)
        reinterpret_cast<sockaddr_in*>(&out)->sin_port = htons(port);
    else
        reinterpret_cast<sockaddr_in6*>(&out)->sin6_port = htons(port);
    freeaddrinfo(result);
    return true;
}
} // namespace

class QuicClientTransport final : public IQuicClientTransport, private QuicTransportBase {
public:
    explicit QuicClientTransport(std::shared_ptr<IExecutor> executor) {
        auto loop = std::dynamic_pointer_cast<NativeLoop>(executor);
        if (!loop)
            throw std::invalid_argument("quic client requires a NativeLoop executor");
        QuicEngineCore::Params params;
        params.loop = std::move(loop);
        params.engine_flags = 0; // client role
        params.get_ssl_ctx = [](void* peer_ctx, const struct sockaddr*) -> ssl_ctx_st* {
            return static_cast<QuicClientTransport*>(peer_ctx)->ssl_ctx_;
        };
        params.verify_cert = verify_cert_cb;
        params.verify_ctx = &verify_state_;
        core_ = std::make_shared<QuicEngineCore>(std::move(params));
        core_->attach_transport(this);
    }

    ~QuicClientTransport() override {
        core_->attach_transport(nullptr);
        if (ssl_ctx_) {
            SSL_CTX_free(ssl_ctx_);
            ssl_ctx_ = nullptr;
        }
        if (verify_state_.store) {
            X509_STORE_free(verify_state_.store);
            verify_state_.store = nullptr;
        }
        QuicEngineCore::destroy(std::move(core_));
    }

    std::shared_ptr<IExecutor> executor() const override { return core_->loop_ptr(); }

    void async_connect(const std::string& host, uint16_t port, const ClientTlsConfig& config,
                       ConnectHandler handler) override {
        assert(!connect_handler_); // one dial at a time (contract discipline)
        connect_handler_ = std::move(handler);
        connect_host_ = host;
        connect_port_ = port;
        connect_config_ = config;
        auto core = core_;
        run_on_loop(core->loop(), [core] {
            if (auto* self = static_cast<QuicClientTransport*>(core->transport()))
                self->do_connect();
        });
    }

private:
    void do_connect() {
        sockaddr_storage peer{};
        int family = AF_INET;
        if (!resolve_udp(connect_host_, connect_port_, peer, family)) {
            fail_connect(std::errc::host_unreachable);
            return;
        }
        if (!core_->has_socket(family)) {
            std::string err;
            if (!core_->open_socket(family, "", 0, err)) { // ephemeral bind
                fail_connect(std::errc::address_family_not_supported);
                return;
            }
        }
        if (!ssl_ctx_) {
            // Verification name: explicit server_name wins, else the host.
            verify_state_.hostname =
                connect_config_.server_name.empty() ? connect_host_ : connect_config_.server_name;
            ssl_ctx_ = make_client_ssl_ctx(connect_config_, verify_state_);
            if (!ssl_ctx_) {
                fail_connect(std::errc::invalid_argument);
                return;
            }
        }
        // SNI: never for IP literals (RFC 6066).
        const char* sni = is_ip_literal(connect_host_)
                              ? nullptr
                              : (connect_config_.server_name.empty() ? connect_host_.c_str()
                                                                     : connect_config_.server_name.c_str());
        lsquic_conn_t* conn = lsquic_engine_connect(
            core_->engine(), N_LSQVER,
            core_->local_sa(family), reinterpret_cast<const sockaddr*>(&peer),
            /*peer_ctx=*/this, /*conn_ctx=*/nullptr, sni,
            /*base_plpmtu=*/0, /*sess_resume=*/nullptr, 0, /*token=*/nullptr, 0);
        if (!conn) {
            fail_connect(std::errc::connection_refused);
            return;
        }
        // on_new_conn fired synchronously above and registered the wrapper.
        core_->kick(); // flush the ClientHello, arm the tick cycle
    }

    void fail_connect(std::errc reason) {
        if (!connect_handler_) return;
        auto loop = core_->loop_ptr();
        auto handler = std::move(connect_handler_);
        connect_handler_ = nullptr;
        std::error_code error = verify_state_.rejected
                                    ? make_error_code(TlsErrc::certificate_verification)
                                    : std::make_error_code(reason);
        loop->post([loop, handler = std::move(handler), error] { handler(error, nullptr); });
    }

    // QuicTransportBase (loop thread)
    void on_new_conn(lsquic_conn_t* conn) override {
        assert(pending_.find(conn) == pending_.end());
        pending_.emplace(conn, std::make_unique<QuicMuxConnection>(core_, conn, /*is_client=*/true));
    }

    void on_hsk_done(lsquic_conn_t* conn, lsquic_hsk_status status) override {
        auto it = pending_.find(conn);
        if (it == pending_.end() || !connect_handler_) return;
        if (status != LSQ_HSK_OK && status != LSQ_HSK_RESUMED_OK)
            return; // failed handshake: reported on the follow-up conn_closed
        auto box = std::make_shared<std::unique_ptr<IMuxConnection>>(std::move(it->second));
        pending_.erase(it);
        auto loop = core_->loop_ptr();
        auto handler = std::move(connect_handler_);
        connect_handler_ = nullptr;
        loop->post([loop, handler = std::move(handler), box]() mutable {
            handler({}, std::move(*box));
        });
    }

    void on_conn_closed(lsquic_conn_t* conn) override {
        auto it = pending_.find(conn);
        if (it == pending_.end()) return;
        pending_.erase(it); // wrapper detached via its own callback
        if (connect_handler_)
            fail_connect(verify_state_.rejected ? std::errc::connection_aborted
                                                : std::errc::connection_refused);
    }

    std::shared_ptr<QuicEngineCore> core_;
    ssl_ctx_st* ssl_ctx_ = nullptr;   // owned; built lazily on first connect
    ClientVerifyState verify_state_;  // owned; address stable for lsquic
    ConnectHandler connect_handler_;
    std::string connect_host_;
    uint16_t connect_port_ = 0;
    ClientTlsConfig connect_config_;
    std::unordered_map<lsquic_conn_t*, std::unique_ptr<QuicMuxConnection>> pending_;
};

} // namespace pr::net::quic

namespace pr::net::quic {

std::shared_ptr<IQuicClientTransport> make_quic_client_transport(
    std::shared_ptr<IExecutor> executor) {
    return std::make_shared<QuicClientTransport>(std::move(executor));
}

} // namespace pr::net::quic
