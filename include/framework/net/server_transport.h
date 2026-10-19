#ifndef PR_FRAMEWORK_NET_SERVER_TRANSPORT_H
#define PR_FRAMEWORK_NET_SERVER_TRANSPORT_H
#include "framework/connection/i_connection.h"
#include "framework/server_config_base.h"

namespace pr::net {
struct TlsConfig {
    std::string certificate_file;
    std::string private_key_file;
    std::string dh_file;
    bool enable_tls1_3 = true;
};
struct ServerTransportConfig {
    std::string address;
    std::vector<ListenerConfig> listeners;
    TlsConfig tls;
};

// An application-level listening service, not an event-loop API. Contexts,
// work guards, acceptors and threads are private implementation details.
class IServerTransport {
public:
    using AcceptHandler = std::function<void(std::unique_ptr<IConnection>, const ListenerConfig&)>;
    virtual ~IServerTransport() = default;
    // Construction binds listeners; port 0 is replaced by the actual bound port.
    virtual const std::vector<ListenerConfig>& listeners() const = 0;
    virtual void start(AcceptHandler handler) = 0;
    // Called externally; returns after accept callbacks have finished and cannot
    // run again. Does not stop injected runtimes or accepted connections.
    virtual void stop() = 0;
    virtual void configure_tls(const TlsConfig& config) = 0;
    // Called on the plain connection's executor after the STARTTLS reply has
    // completed and no other IO is pending. Consumes the transport, preserves
    // its serialized executor, and returns a TLS connection before handshake.
    virtual std::unique_ptr<IConnection> upgrade_tls(std::unique_ptr<IConnection> plain) = 0;
};
class IServerTransportProvider {
public:
    virtual ~IServerTransportProvider() = default;
    virtual std::unique_ptr<IServerTransport> make_server_transport(const ServerTransportConfig& config) = 0;
};
} // namespace pr::net
#endif
