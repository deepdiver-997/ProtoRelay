#include "quic_internal.h"
#include "framework/net/server_transport.h" // TlsConfig definition

#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/err.h>

#include <arpa/inet.h>
#include <mutex>
#include <stdexcept>

namespace pr::net::quic {

void ensure_global_init() {
    static std::once_flag once;
    std::call_once(once, [] {
        // call_once blocks concurrent callers until init finished — a bare
        // "already started?" flag would let a second caller return early.
        if (0 != lsquic_global_init(LSQUIC_GLOBAL_SERVER | LSQUIC_GLOBAL_CLIENT))
            throw std::runtime_error("quic: lsquic_global_init failed");
    });
}

// Load every PEM certificate in `bio` into the store.
static bool add_pem_certs(X509_STORE* store, BIO* bio) {
    while (X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)) {
        if (X509_STORE_add_cert(store, cert) != 1) {
            X509_free(cert);
            return false;
        }
        X509_free(cert);
    }
    // PEM_R_NO_START_LINE at EOF is the normal exit; other errors are real.
    unsigned long err = ERR_peek_last_error();
    ERR_clear_error();
    return ERR_GET_REASON(err) == PEM_R_NO_START_LINE || ERR_peek_last_error() == 0;
}

ssl_ctx_st* make_server_ssl_ctx(const TlsConfig& config) {
    ensure_global_init();
    if (config.certificate_file.empty() || config.private_key_file.empty())
        return nullptr;
    ssl_ctx_st* ctx = SSL_CTX_new(TLS_method());
    if (!ctx) return nullptr;
    if (SSL_CTX_use_certificate_chain_file(ctx, config.certificate_file.c_str()) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx, config.private_key_file.c_str(), SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(ctx);
        return nullptr;
    }
    // QUIC mandates TLS 1.3; TlsConfig::dh_file / enable_tls1_3 do not apply.
    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
    SSL_CTX_set_alpn_select_cb(
        ctx,
        [](SSL*, const unsigned char** out, unsigned char* out_len,
           const unsigned char* in, unsigned int in_len, void*) {
            if (SSL_select_next_proto(const_cast<unsigned char**>(out), out_len,
                                      in, in_len,
                                      reinterpret_cast<const unsigned char*>(kAlpnWire),
                                      sizeof(kAlpnWire) - 1) == OPENSSL_NPN_NEGOTIATED)
                return SSL_TLSEXT_ERR_OK;
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        },
        nullptr);
    return ctx;
}

static X509_STORE* build_verify_store(const ClientTlsConfig& config) {
    X509_STORE* store = X509_STORE_new();
    if (!store) return nullptr;
    bool have_trust = false;
    if (!config.ca_file.empty()) {
        BIO* bio = BIO_new_file(config.ca_file.c_str(), "r");
        if (bio) {
            have_trust = add_pem_certs(store, bio);
            BIO_free(bio);
        }
        if (!have_trust) { X509_STORE_free(store); return nullptr; }
    }
    if (!config.ca_pem.empty()) {
        BIO* bio = BIO_new_mem_buf(config.ca_pem.data(),
                                   static_cast<int>(config.ca_pem.size()));
        if (bio) {
            have_trust = add_pem_certs(store, bio);
            BIO_free(bio);
        }
        if (!have_trust) { X509_STORE_free(store); return nullptr; }
    }
    if (config.use_system_ca) {
        if (X509_STORE_set_default_paths(store) != 1) {
            X509_STORE_free(store);
            return nullptr;
        }
        have_trust = true;
    }
    if (!have_trust) { X509_STORE_free(store); return nullptr; }
    return store;
}

ssl_ctx_st* make_client_ssl_ctx(const ClientTlsConfig& config, ClientVerifyState& verify) {
    ensure_global_init();
    verify.store = build_verify_store(config);
    if (!verify.store) return nullptr;
    // Verification name: explicit server_name wins; otherwise the connect
    // host is used unless it is an IP literal (RFC 6066: no SNI for IPs —
    // verification still happens, against the IP below).
    verify.hostname = config.server_name;

    ssl_ctx_st* ctx = SSL_CTX_new(TLS_method());
    if (!ctx) return nullptr;
    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
    SSL_CTX_set_alpn_protos(ctx, reinterpret_cast<const unsigned char*>(kAlpnWire),
                            sizeof(kAlpnWire) - 1);
    return ctx;
}

int verify_cert_cb(void* verify_ctx, struct stack_st_X509* chain) {
    auto* state = static_cast<ClientVerifyState*>(verify_ctx);
    X509* leaf = sk_X509_value(chain, 0);
    if (!leaf) { state->rejected = true; return -1; }

    X509_STORE_CTX* store_ctx = X509_STORE_CTX_new();
    if (!store_ctx) { state->rejected = true; return -1; }
    // The whole peer chain acts as untrusted intermediates; the store holds
    // the trust anchors.
    X509_STORE_CTX_init(store_ctx, state->store, leaf, chain);
    int ok = X509_verify_cert(store_ctx);
    unsigned long err = X509_STORE_CTX_get_error(store_ctx);
    X509_STORE_CTX_free(store_ctx);
    if (ok != 1) { state->rejected = true; return -1; }

    if (!state->hostname.empty()) {
        in_addr v4{};
        in6_addr v6{};
        int match;
        if (inet_pton(AF_INET, state->hostname.c_str(), &v4) == 1)
            match = X509_check_ip(leaf, reinterpret_cast<const uint8_t*>(&v4), sizeof(v4), 0);
        else if (inet_pton(AF_INET6, state->hostname.c_str(), &v6) == 1)
            match = X509_check_ip(leaf, reinterpret_cast<const uint8_t*>(&v6), sizeof(v6), 0);
        else
            match = X509_check_host(leaf, state->hostname.c_str(),
                                    state->hostname.size(), 0, nullptr);
        if (match != 1) { state->rejected = true; return -1; }
    }
    return 0;
}

MuxCloseInfo map_conn_status(int status, const char* errbuf, bool local_close) {
    if (local_close) return {std::error_code{}, "local close"};
    const char* detail = (errbuf && errbuf[0]) ? errbuf : "connection closed";
    switch (status) {
    case LSCONN_ST_CLOSED:
    case LSCONN_ST_PEER_GOING_AWAY:
        return {make_error_code(IoErrc::end_of_stream), detail};
    case LSCONN_ST_TIMED_OUT:
        return {std::make_error_code(std::errc::timed_out), detail};
    case LSCONN_ST_RESET:
        return {std::make_error_code(std::errc::connection_reset), detail};
    case LSCONN_ST_VERNEG_FAILURE:
        return {std::make_error_code(std::errc::connection_refused), detail};
    case LSCONN_ST_GOING_AWAY:   // our own GOAWAY-equivalent path
    case LSCONN_ST_USER_ABORTED:
        return {std::error_code{}, detail};
    case LSCONN_ST_ERROR:
    default:
        return {std::make_error_code(std::errc::connection_aborted), detail};
    }
}

} // namespace pr::net::quic
