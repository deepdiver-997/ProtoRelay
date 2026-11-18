#ifndef PR_FRAMEWORK_NET_NATIVE_TRANSPORT_H
#define PR_FRAMEWORK_NET_NATIVE_TRANSPORT_H
// Native (non-Asio) TCP transport factories. Optional backend satisfying the
// same IConnection/IExecutor/ITimer/IServerTransport/IClientTransport contracts
// as the default Asio adapters.
//
// Scope: plain TCP listeners/connections on POSIX/kqueue platforms, plus TLS
// when the optional protorelay_native_tls module installed its hooks (see
// native_tls.h: SSL listeners, configure_tls, upgrade_tls, async_start_tls).
// Without that module every TLS entry point throws operation_not_supported.
// No Windows, no QUIC.
#include "framework/net/client_transport.h"
#include "framework/net/native_runtime.h"
#include "framework/net/server_transport.h"

namespace pr::net::native {

class NativeRuntime;

// Listener ownership and accepted-connection ownership are independent. New
// connections round-robin across loops, using the optional runtime cursor.
// Port 0 is replaced by the actual bound port. The transport keeps loops alive
// without retaining the runtime; delayed deliveries do not own their target loop.
std::unique_ptr<net::IServerTransport> make_native_server_transport(
    const net::ServerTransportConfig& config, const std::vector<NativeLoopPtr>& loops,
    std::shared_ptr<std::atomic<size_t>> connection_cursor = {});

// Minimal client connection factory (plain TCP, numeric or resolved hosts).
std::shared_ptr<net::IClientTransport> make_native_client_transport(
    std::shared_ptr<net::IExecutor> executor);

} // namespace pr::net::native
#endif
