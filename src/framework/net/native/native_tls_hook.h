#ifndef PR_FRAMEWORK_NET_NATIVE_NATIVE_TLS_HOOK_H
#define PR_FRAMEWORK_NET_NATIVE_NATIVE_TLS_HOOK_H
// Seam between the OpenSSL-free native backend and the optional
// protorelay_native_tls module. The backend consults this registry; a null
// hook set means "no TLS in this binary" and every TLS entry point keeps its
// fail-closed behavior. No OpenSSL types may appear here.
#include "framework/net/client_transport.h"
#include "native_stream_internal.h"

namespace pr::net::native::tls_hook {

class Hooks {
public:
    virtual ~Hooks() = default;
    // Type-erased adapter-owned context (SSL_CTX plus client settings inside
    // the protorelay_native_tls module).
    using Context = std::shared_ptr<void>;
    // Server context from certificate/key files. Throws (std::system_error /
    // std::runtime_error) when a file is unreadable or a setting is invalid.
    virtual Context make_server_context(const net::TlsConfig& config) = 0;
    // Client context. Peer chain and hostname verification are mandatory and
    // cannot be disabled; throws when no trust source is configured.
    virtual Context make_client_context(const net::ClientTlsConfig& config) = 0;
    // Consume an open plain stream into a TLS connection on the stream's own
    // loop (executor preserved). No IO happens here; the returned connection
    // performs the SSL handshake when its async_handshake(role) is driven,
    // and role must match the connection kind (server wrap = server role).
    virtual std::unique_ptr<IConnection> wrap(const NativeLoopPtr& loop,
                                                   const std::shared_ptr<StreamState>& stream,
                                                   const Context& context,
                                                   net::HandshakeRole role) = 0;
};

// Installs the hook set. Idempotent overwrites are allowed; nullptr restores
// fail-closed behavior. Not thread-safe — call before transports are created.
void set_hooks(Hooks* hooks);
// Never null-unsafe: returns nullptr when the TLS module is absent.
Hooks* get_hooks();

} // namespace pr::net::native::tls_hook
#endif
