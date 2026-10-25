#ifndef PR_PROTORELAY_QUIC_CORE_H
#define PR_PROTORELAY_QUIC_CORE_H
// QUIC engine host: one lsquic_engine_t, its UDP sockets and the advisory
// tick timer, all bound to one NativeLoop (see docs/architecture/quic-backend.md
// for the threading rationale). Loop-thread only except where noted.
//
// The lsquic stream_if context is the core itself, not the transport: the core
// is kept alive by every delivered connection wrapper, while the transport may
// be destroyed at any time (its pointer is cleared on the loop thread).
#include "framework/net/native_runtime.h"
#include "quic_internal.h"

#include <sys/socket.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <lsquic.h>
}

namespace pr::net::quic {

class QuicTransportBase;

class QuicEngineCore {
public:
    // lsquic declares the cert-lookup callback as a typedef; the ssl-ctx and
    // verify-callback types are inline in lsquic_engine_api.
    using LookupCertF = ssl_ctx_st* (*)(void*, const struct sockaddr*, const char*);
    using GetSslCtxF = ssl_ctx_st* (*)(void* peer_ctx, const struct sockaddr* local);
    using VerifyCertF = int (*)(void* verify_ctx, struct stack_st_X509* chain);

    struct Params {
        std::shared_ptr<NativeLoop> loop;
        unsigned engine_flags = 0; // LSENG_SERVER for the listener role
        LookupCertF lookup_cert = nullptr; // server: by-SNI ctx lookup
        void* cert_lu_ctx = nullptr;
        GetSslCtxF get_ssl_ctx = nullptr;
        VerifyCertF verify_cert = nullptr; // client only
        void* verify_ctx = nullptr;
        void* packet_peer_ctx = nullptr; // conn peer_ctx handed to get_ssl_ctx
    };

    explicit QuicEngineCore(Params params);
    ~QuicEngineCore();

    QuicEngineCore(const QuicEngineCore&) = delete;
    QuicEngineCore& operator=(const QuicEngineCore&) = delete;

    // The transport's callbacks (on_new_conn/on_hsk_done/on_conn_closed).
    // Cleared — on the loop thread — when the transport is destroyed.
    void attach_transport(QuicTransportBase* transport);
    QuicTransportBase* transport() const { return transport_; }

    // Advance the engine now and re-arm the advisory tick (start()/connect()).
    void kick();
    lsquic_engine_t* engine() const { return engine_; }
    const std::shared_ptr<NativeLoop>& loop_ptr() const { return loop_; }

    // Binds `address:port` (family from the parsed address; empty = any) or,
    // for the client, an ephemeral socket of the given family. Loop thread
    // only. A bound port of 0 is replaced by the actually bound port in the
    // cached local sockaddr. Returns false with `err` set.
    bool open_socket(int family, const std::string& address, uint16_t port, std::string& err);
    void close_sockets();
    bool has_socket(int family) const;
    const struct sockaddr* local_sa(int family) const;

    // Marshal destruction onto the loop; callable from any thread. Shared
    // ownership: delivered connection wrappers keep the core (and its loop)
    // alive independently of the transport.
    static void destroy(std::shared_ptr<QuicEngineCore> core);

    NativeLoop& loop() { return *loop_; }

private:
    static constexpr int kIndexV4 = 0, kIndexV6 = 1;
    static int index_of(int family) { return family == AF_INET6 ? kIndexV6 : kIndexV4; }

    // lsquic callbacks (static table, ctx is the core).
    static int packets_out_cb(void* ctx, const lsquic_out_spec* specs, unsigned count);
    void on_socket_event(int index, unsigned ready);
    void drain_socket(int index);
    void schedule_tick();
    void enable_write_watch(int index);
    void disable_write_watch(int index);

    std::shared_ptr<NativeLoop> loop_;
    Params params_;
    QuicTransportBase* transport_ = nullptr;
    lsquic_engine_t* engine_ = nullptr;
    struct Socket {
        int fd = -1;
        sockaddr_storage local{};
        std::shared_ptr<LoopEntry> entry;
        std::vector<unsigned char> recv_buf = std::vector<unsigned char>(65536);
        sockaddr_storage from{};
    };
    std::array<Socket, 2> sockets_{};
    bool write_watch_[2] = {false, false};
    std::shared_ptr<ITimer> tick_timer_;
};

} // namespace pr::net::quic

#endif // PR_PROTORELAY_QUIC_CORE_H
