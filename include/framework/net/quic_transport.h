#ifndef PR_FRAMEWORK_NET_QUIC_TRANSPORT_H
#define PR_FRAMEWORK_NET_QUIC_TRANSPORT_H
// QUIC transport contracts: multiplexed connections. A QUIC connection is a
// container of bidirectional streams; every stream satisfies the IConnection
// contract unchanged (ordered byte stream, serialized executor, never-inline
// exactly-once completions, caller-owned buffers, documented cancellation).
// Nothing here names a TLS or QUIC library type: TLS configuration reuses the
// TlsConfig / ClientTlsConfig pure-data structures, and the lsquic-backed
// implementation lives entirely inside the optional protorelay_quic module.
// Framework-only and contracts-only builds compile this header without that
// module; its factory symbols below then simply do not link, mirroring the
// native_tls.h discipline.
// The TLS option structures are reused but only by reference, so this header
// needs no more than the std-only client_transport.h chain: TlsConfig (which
// server_transport.h defines, together with nlohmann/json) is declared below
// and must be complete at the call site.
#include "framework/connection/i_connection.h"
#include "framework/net/client_transport.h"

namespace pr::net {

struct TlsConfig; // server_transport.h; passed by reference only here

// Post-mortem information for an ended connection. Carried as generic codes;
// detail preserves the wire reason phrase where the transport provides one.
struct MuxCloseInfo {
    std::error_code code;   // {} = clean local close, end_of_stream = peer's
    std::string detail;     // reason phrase / diagnostic, best effort
};

// One QUIC connection: opens and accepts streams. Everything is initiated on
// executor() and every completion — including failures — returns to that same
// serialized executor, never inline, exactly once. Streams handed out run on
// that executor too. close/cancel also initiate on executor().
class IMuxConnection {
public:
    // Completion of one multiplexed-stream event (open or accept). On success
    // the handler takes ownership of the new stream. Error cases:
    //   - operation_canceled: the pending operation was canceled via
    //     cancel(), or the connection was closed locally first.
    //   - end_of_stream: the connection ended without delivering a stream
    //     (peer closed, handshake or transport failure). close_reason() on
    //     the connection explains why.
    //   - any other std::error_code: transport failure mapped to a generic
    //     std::errc code; the connection has ended either way.
    using StreamHandler = std::function<void(const std::error_code&, std::unique_ptr<IConnection>)>;

    virtual ~IMuxConnection() = default;

    // Initiate one bidirectional stream. At most one open_stream may be
    // pending: call again only after the previous handler ran. Streams that
    // are already open carry concurrent IO independently — multiplexing
    // happens across streams, not across pending opens.
    virtual void open_stream(StreamHandler handler) = 0;

    // Receive the next peer-initiated stream. Suspends while none has
    // arrived; cancellation follows IConnection::async_read semantics. At
    // most one pending accept_stream: streams opened by the peer meanwhile
    // are held by the connection and delivered to the next call. At most one
    // pending open_stream and one pending accept_stream at a time.
    virtual void accept_stream(StreamHandler handler) = 0;

    // Post-mortem: why the connection ended. Valid once any completion
    // reported its end (or after close() was issued); {end_of_stream} means
    // the peer closed. Empty while the connection is healthy.
    virtual MuxCloseInfo close_reason() const = 0;

    // Connection-level shutdown: flush queued egress and send the close.
    // Pending open_stream/accept_stream complete with operation_canceled;
    // pending IO on delivered streams completes with operation_canceled and
    // further IO initiation fails the same way. Does not wait for the peer's
    // acknowledgement. Repeated calls are no-ops.
    virtual void close() = 0;

    // Cancels the pending open_stream/accept_stream only — like
    // IConnection::cancel it does not close the connection. The displaced
    // handler runs with operation_canceled exactly once.
    virtual void cancel() = 0;

    virtual bool is_open() const = 0;

    // The serialized executor every completion on this connection and its
    // streams runs on; watchdog timers come from here too.
    virtual std::shared_ptr<IExecutor> executor() const = 0;
};

// Pure-data listener configuration for one QUIC endpoint. The TLS identity
// is configured separately (configure_tls), mirroring IServerTransport.
struct QuicServerConfig {
    std::string address;    // UDP bind address ("127.0.0.1", "::", ...)
    uint16_t port = 0;      // 0 binds an ephemeral port; replaced on bind
};

// Application-level QUIC listener: accepts whole connections and delivers
// each as an IMuxConnection. The UDP socket, the QUIC engine and its driving
// loop are private implementation details.
class IQuicServerTransport {
public:
    using MuxAcceptHandler = std::function<void(std::unique_ptr<IMuxConnection>)>;
    virtual ~IQuicServerTransport() = default;
    // Construction binds the UDP socket; port 0 is replaced by the actually
    // bound port (IServerTransport::listeners() discipline).
    virtual const QuicServerConfig& config() const = 0;
    // TLS identity for accepted handshakes (certificate and private key).
    // QUIC mandates TLS 1.3, so TlsConfig::dh_file and enable_tls1_3 are
    // ignored. At most once, before start().
    virtual void configure_tls(const TlsConfig& tls) = 0;
    // Begins accepting. A connection is delivered only once its handshake
    // completed: fully established and ready for open/accept. Failed
    // handshakes are discarded; the handler runs on the delivered
    // connection's executor(), never inline.
    virtual void start(MuxAcceptHandler handler) = 0;
    // Stops listening; returns after queued handlers have finished and
    // cannot run again. Does not stop already-delivered connections.
    virtual void stop() = 0;
};

// QUIC client dialer for one endpoint at a time. Certificate and hostname
// verification are mandatory (async_start_tls discipline; verification
// cannot be disabled).
class IQuicClientTransport {
public:
    using ConnectHandler = std::function<void(const std::error_code&, std::unique_ptr<IMuxConnection>)>;
    virtual ~IQuicClientTransport() = default;
    virtual std::shared_ptr<IExecutor> executor() const = 0;
    // The QUIC handshake embeds TLS 1.3, so success means established: the
    // delivered connection accepts open_stream immediately. Initiated on
    // executor(), exactly one completion on that executor, one dial at a
    // time; redial after the handler ran (either outcome). Failures map to
    // generic std::errc codes (connection_refused, timed_out, ...).
    virtual void async_connect(const std::string& host, uint16_t port,
                               const ClientTlsConfig& config,
                               ConnectHandler handler) = 0;
};

// Implemented by the optional protorelay_quic module: present only when the
// vendored third-party QUIC/TLS libraries are part of the build. All
// completions and delivered-connection IO run on the executor passed here.
namespace quic {
std::unique_ptr<IQuicServerTransport> make_quic_server_transport(
    const QuicServerConfig& config, std::shared_ptr<IExecutor> executor);
std::shared_ptr<IQuicClientTransport> make_quic_client_transport(
    std::shared_ptr<IExecutor> executor);
} // namespace pr::net::quic

} // namespace pr::net

#endif // PR_FRAMEWORK_NET_QUIC_TRANSPORT_H
