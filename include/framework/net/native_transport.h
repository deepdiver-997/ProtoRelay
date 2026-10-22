#ifndef PR_FRAMEWORK_NET_NATIVE_TRANSPORT_H
#define PR_FRAMEWORK_NET_NATIVE_TRANSPORT_H
// Native (non-Asio) TCP transport factories. Parallel optional module: nothing
// in the production services is wired to these; they satisfy the same
// IConnection/IExecutor/ITimer/IServerTransport/IClientTransport contracts as
// the Asio adapters.
//
// v1 scope: plain TCP, POSIX/kqueue platforms. No TLS, no Windows, no QUIC —
// upgrade_tls / configure_tls / SSL listeners / async_start_tls throw
// operation_not_supported.
#include "framework/net/client_transport.h"
#include "framework/net/native_runtime.h"
#include "framework/net/server_transport.h"

namespace pr::net::native {

class NativeRuntime;

// Binds (and, on start, accepts) the configured plain-TCP listeners round-robin
// on the given loops (as provided by a NativeRuntime). Port 0 is replaced by
// the actually bound port. The transport keeps the loops alive; the runtime
// object itself may be released independently.
std::unique_ptr<net::IServerTransport> make_native_server_transport(
    const net::ServerTransportConfig& config, const std::vector<NativeLoopPtr>& loops);

// Minimal client connection factory (plain TCP, numeric or resolved hosts).
std::shared_ptr<net::IClientTransport> make_native_client_transport(
    std::shared_ptr<net::IExecutor> executor);

} // namespace pr::net::native
#endif
