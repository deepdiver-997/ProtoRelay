#pragma once
#include "framework/connection/i_connection.h"

namespace pr::net {
// Configuration contains data only; TLS library objects stay inside adapters.
// Peer certificate and hostname verification are mandatory.
struct ClientTlsConfig {
    std::string server_name;
    std::string ca_file;
    std::string ca_pem;
    bool use_system_ca = true;
};

// All operations are initiated on executor(), and every completion returns to
// that same serialized executor. A transport can reconnect after close(); the
// implementation retains cancelled operation state until its callbacks finish.
class IClientTransport : public IConnection {
public:
    virtual void async_connect(const std::string& host, uint16_t port,
                               HandshakeHandler handler) = 0;
    // Upgrade an idle connected plain transport and complete the client TLS
    // handshake. No read/write may be pending; verification cannot be disabled.
    virtual void async_start_tls(ClientTlsConfig config, HandshakeHandler handler) = 0;
};
class IClientTransportProvider {
public:
    virtual ~IClientTransportProvider() = default;
    virtual std::shared_ptr<IClientTransport> make_client_transport() = 0;
};
} // namespace pr::net
