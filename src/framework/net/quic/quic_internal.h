#ifndef PR_PROTORELAY_QUIC_INTERNAL_H
#define PR_PROTORELAY_QUIC_INTERNAL_H
// Internal shared pieces of the protorelay_quic module. Includes lsquic and
// BoringSSL headers — this header must never be included from the public
// contract surface (include/framework/net/quic_transport.h) or by anything
// outside this module directory.
//
// Everything here is BoringSSL-symbol-prefixed: the module (and its tests)
// compile with BORINGSSL_PREFIX=pr_bssl_ so <openssl/*.h> declares the
// renamed API that the vendored libraries were built against.
#include "framework/net/native_runtime.h"
#include "framework/net/quic_transport.h"

extern "C" {
#include <lsquic.h>
}
#include <openssl/ssl.h>

#include <memory>
#include <string>

namespace pr::net::quic {

using native::NativeLoop;
using native::LoopEntry;
using native::NativeTimer;
using native::Ready;

// lsquic_global_init once, both roles; throws std::runtime_error on failure.
void ensure_global_init();

// Length-prefixed ALPN wire list ("prq-v1" = 6 bytes); both engines must agree.
inline constexpr char kAlpnWire[] = "\x06prq-v1";

// TLS 1.3-only server context: certificate chain, private key, ALPN select.
// Returns nullptr (caller throws) when the files cannot be loaded.
ssl_ctx_st* make_server_ssl_ctx(const TlsConfig& config);

// Client-side certificate verification state, built from ClientTlsConfig and
// owned by the transport for the engine's lifetime. `rejected` is set by the
// ea_verify_cert callback when the chain or hostname does not verify.
struct ClientVerifyState {
    X509_STORE* store = nullptr; // owned; built once per transport
    std::string hostname;        // SNI / verification name (host or server_name)
    bool rejected = false;
};
ssl_ctx_st* make_client_ssl_ctx(const ClientTlsConfig& config, ClientVerifyState& verify);

// ea_verify_cert: chain + hostname against the prepared store. Records
// `rejected` so the handshake failure can be reported as
// TlsErrc::certificate_verification.
int verify_cert_cb(void* verify_ctx, struct stack_st_X509* chain);

// LSQUIC_CONN_STATUS -> contract close reason. `local_close` wins (the caller
// already recorded a clean local shutdown before the status arrived).
MuxCloseInfo map_conn_status(int status, const char* errbuf, bool local_close);

// Run f on the loop thread: inline when already there, queued while the loop
// lives, inline when it is gone (nothing left to race with). f is passed as
// an lvalue on purpose: invoke_when_live copies it into its task, so the
// inline fallback below never runs over a moved-from capture.
template <typename F>
void run_on_loop(NativeLoop& loop, F&& f) {
    if (loop.is_loop_thread() || !loop.invoke_when_live(f)) {
        std::forward<F>(f)();
    }
}

} // namespace pr::net::quic

#endif // PR_PROTORELAY_QUIC_INTERNAL_H
