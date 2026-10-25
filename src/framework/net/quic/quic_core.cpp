#include "quic_core.h"
#include "quic_transport_impl.h"

#include <arpa/inet.h>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <stdexcept>

namespace pr::net::quic {

// ── lsquic stream_if routing (static table; ctx is the QuicEngineCore) ──
//
// NULL-stream deliveries (make_stream refused because the connection is going
// away) cannot be routed through lsquic_stream_conn — the open/accept pendings
// were already displaced by the wrapper's close/conn-closed path, so they are
// dropped here.

static lsquic_conn_ctx_t* on_new_conn_cb(void* ctx, lsquic_conn_t* conn) {
    auto* core = static_cast<QuicEngineCore*>(ctx);
    if (core->transport()) core->transport()->on_new_conn(conn);
    return static_cast<lsquic_conn_ctx_t*>(lsquic_conn_get_ctx(conn));
}

static void on_conn_closed_cb(lsquic_conn_t* conn) {
    // No ctx parameter: the wrapper (reachable via the conn ctx) notifies the
    // transport itself. The wrapper may already be detached (application
    // destroyed the delivered connection); lsquic only guarantees the ctx is
    // readable until this callback returns.
    if (auto* wrapper = reinterpret_cast<QuicMuxConnection*>(lsquic_conn_get_ctx(conn)))
        wrapper->on_conn_closed();
}

static lsquic_stream_ctx_t* on_new_stream_cb(void*, lsquic_stream_t* stream) {
    if (!stream) return nullptr; // going-away notification, already displaced
    auto* conn = reinterpret_cast<QuicMuxConnection*>(lsquic_conn_get_ctx(lsquic_stream_conn(stream)));
    if (!conn) return nullptr; // connection wrapper detached
    return reinterpret_cast<lsquic_stream_ctx_t*>(conn->on_new_stream(stream));
}

static void on_read_cb(lsquic_stream_t* stream, lsquic_stream_ctx_t* h) {
    if (h) reinterpret_cast<QuicStream*>(h)->on_readable();
}

static void on_write_cb(lsquic_stream_t* stream, lsquic_stream_ctx_t* h) {
    if (h) reinterpret_cast<QuicStream*>(h)->on_writeable();
}

static void on_close_cb(lsquic_stream_t* stream, lsquic_stream_ctx_t* h) {
    if (h) reinterpret_cast<QuicStream*>(h)->on_closed();
}

static void on_reset_cb(lsquic_stream_t* stream, lsquic_stream_ctx_t* h, int how) {
    if (h) reinterpret_cast<QuicStream*>(h)->on_reset(how);
}

// on_hsk_done carries no stream_if_ctx: resolve the transport through the
// conn wrapper (see QuicMuxConnection::on_hsk_done).
static void on_hsk_done_cb(lsquic_conn_t* conn, lsquic_hsk_status status) {
    if (auto* wrapper = reinterpret_cast<QuicMuxConnection*>(lsquic_conn_get_ctx(conn)))
        wrapper->on_hsk_done(status);
}

static lsquic_stream_if make_stream_if() {
    lsquic_stream_if si = {};
    si.on_new_conn    = on_new_conn_cb;
    si.on_conn_closed = on_conn_closed_cb;
    si.on_new_stream  = on_new_stream_cb;
    si.on_read        = on_read_cb;
    si.on_write       = on_write_cb;
    si.on_close       = on_close_cb;
    si.on_hsk_done    = on_hsk_done_cb;
    si.on_reset       = on_reset_cb;
    return si;
}

const lsquic_stream_if& stream_if_table() {
    static const lsquic_stream_if table = make_stream_if();
    return table;
}

// ── QuicEngineCore ──

QuicEngineCore::QuicEngineCore(Params params) : loop_(std::move(params.loop)), params_(std::move(params)) {
    ensure_global_init();
    lsquic_engine_api api = {};
    api.ea_stream_if       = &stream_if_table();
    api.ea_stream_if_ctx   = this;
    api.ea_packets_out     = packets_out_cb;
    api.ea_packets_out_ctx = this;
    api.ea_lookup_cert     = params_.lookup_cert;
    api.ea_cert_lu_ctx     = params_.cert_lu_ctx;
    api.ea_get_ssl_ctx     = params_.get_ssl_ctx;
    api.ea_verify_cert     = params_.verify_cert;
    api.ea_verify_ctx      = params_.verify_ctx;

    lsquic_engine_settings settings;
    lsquic_engine_init_settings(&settings, params_.engine_flags);
    api.ea_settings = &settings;

    engine_ = lsquic_engine_new(params_.engine_flags, &api);
    if (!engine_) throw std::runtime_error("quic: lsquic_engine_new failed");
    tick_timer_ = loop_->make_timer();
}

QuicEngineCore::~QuicEngineCore() {
    // Loop thread only (guaranteed by destroy()).
    if (tick_timer_) {
        tick_timer_->cancel();
        tick_timer_.reset();
    }
    close_sockets();
    if (engine_) {
        lsquic_engine_destroy(engine_);
        engine_ = nullptr;
    }
}

void QuicEngineCore::attach_transport(QuicTransportBase* transport) {
    run_on_loop(*loop_, [this, transport] { transport_ = transport; });
}

void QuicEngineCore::kick() {
    if (!engine_) return;
    lsquic_engine_process_conns(engine_);
    schedule_tick();
}

void QuicEngineCore::destroy(std::shared_ptr<QuicEngineCore> core) {
    NativeLoop& loop = *core->loop_;
    run_on_loop(loop, [core] { /* destructor runs here */ });
}

bool QuicEngineCore::open_socket(int family, const std::string& address, uint16_t port, std::string& err) {
    int index = index_of(family);
    if (sockets_[index].fd >= 0) return true;

    int fd = ::socket(family, SOCK_DGRAM, 0);
    if (fd < 0) { err = "socket() failed"; return false; }
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    const int fd_flags = ::fcntl(fd, F_GETFD, 0);
    if (fd_flags >= 0) ::fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);

    sockaddr_storage sa{};
    if (family == AF_INET) {
        auto* sin = reinterpret_cast<sockaddr_in*>(&sa);
        sin->sin_family = AF_INET;
        sin->sin_port = htons(port);
        if (address.empty()) sin->sin_addr.s_addr = INADDR_ANY;
        else if (inet_pton(AF_INET, address.c_str(), &sin->sin_addr) != 1) {
            ::close(fd); err = "invalid IPv4 bind address: " + address; return false;
        }
    } else {
        auto* sin6 = reinterpret_cast<sockaddr_in6*>(&sa);
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons(port);
        if (address.empty()) sin6->sin6_addr = in6addr_any;
        else if (inet_pton(AF_INET6, address.c_str(), &sin6->sin6_addr) != 1) {
            ::close(fd); err = "invalid IPv6 bind address: " + address; return false;
        }
    }
    socklen_t sa_len = family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&sa), sa_len) != 0) {
        int saved = errno;
        ::close(fd);
        err = std::string("bind() failed: ") + std::strerror(saved);
        return false;
    }
    socklen_t local_len = sizeof(sockets_[index].local);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&sockets_[index].local), &local_len) != 0) {
        int saved = errno;
        ::close(fd);
        err = std::string("getsockname() failed: ") + std::strerror(saved);
        return false;
    }

    sockets_[index].fd = fd;
    auto entry = std::make_shared<LoopEntry>();
    entry->on_event = [this, index](unsigned ready) { on_socket_event(index, ready); };
    entry->on_loop_stopping = [] {
        // The fd is closed by the core destructor; nothing to react to here.
    };
    sockets_[index].entry = entry;
    if (!loop_->add_fd(fd, Ready::readable, entry)) {
        ::close(fd);
        sockets_[index].fd = -1;
        err = "add_fd() failed";
        return false;
    }
    return true;
}

void QuicEngineCore::close_sockets() {
    for (int i = 0; i < 2; ++i) {
        if (sockets_[i].fd >= 0) {
            loop_->remove_fd(sockets_[i].fd);
            ::close(sockets_[i].fd);
            sockets_[i].fd = -1;
            sockets_[i].entry.reset();
        }
        write_watch_[i] = false;
    }
}

bool QuicEngineCore::has_socket(int family) const { return sockets_[index_of(family)].fd >= 0; }

const struct sockaddr* QuicEngineCore::local_sa(int family) const {
    return reinterpret_cast<const sockaddr*>(&sockets_[index_of(family)].local);
}

void QuicEngineCore::on_socket_event(int index, unsigned ready) {
    if (ready & Ready::readable) drain_socket(index);
    if ((ready & Ready::writable) && write_watch_[index]) {
        write_watch_[index] = false;
        disable_write_watch(index);
        if (engine_) lsquic_engine_send_unsent_packets(engine_);
    }
}

void QuicEngineCore::drain_socket(int index) {
    auto& socket = sockets_[index];
    if (socket.fd < 0 || !engine_) return;
    bool got_packet = false;
    for (;;) {
        socklen_t from_len = sizeof(socket.from);
        ssize_t n = ::recvfrom(socket.fd, socket.recv_buf.data(), socket.recv_buf.size(),
                               0, reinterpret_cast<sockaddr*>(&socket.from), &from_len);
        if (n < 0) {
            if (errno == EINTR) continue;
            break; // EAGAIN / transient socket error: resume on the next event
        }
        if (n == 0) break;
        got_packet = true;
        int r = lsquic_engine_packet_in(
            engine_, socket.recv_buf.data(), static_cast<size_t>(n),
            reinterpret_cast<const sockaddr*>(&socket.local),
            reinterpret_cast<const sockaddr*>(&socket.from),
            params_.packet_peer_ctx, 0 /* ecn */);
    }
    if (got_packet) {
        lsquic_engine_process_conns(engine_);
        schedule_tick();
    }
}

int QuicEngineCore::packets_out_cb(void* ctx, const lsquic_out_spec* specs, unsigned count) {
    auto* core = static_cast<QuicEngineCore*>(ctx);
    unsigned sent = 0;
    bool blocked = false;
    for (unsigned i = 0; i < count; ++i) {
        const auto& spec = specs[i];
        // Send straight from the engine thread on the matching family's fd;
        // single-datagram sendmsg is kernel-atomic (see design doc). A batch
        // may mix families.
        int index = spec.dest_sa->sa_family == AF_INET6 ? kIndexV6 : kIndexV4;
        int fd = core->sockets_[index].fd;
        if (fd < 0) continue; // family unavailable: spec stays unsent

        struct msghdr header = {};
        header.msg_name = const_cast<struct sockaddr*>(spec.dest_sa);
        header.msg_namelen = spec.dest_sa->sa_family == AF_INET
                                 ? sizeof(sockaddr_in)
                                 : sizeof(sockaddr_in6);
        header.msg_iov = const_cast<struct iovec*>(spec.iov);
        header.msg_iovlen = spec.iovlen;

        ssize_t n = ::sendmsg(fd, &header, 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            blocked = true; // lsquic retains the rest for send_unsent_packets
            break;
        }
        if (n >= 0) ++sent;
    }
    if (blocked && sent < count)
        core->enable_write_watch(specs[sent].dest_sa->sa_family == AF_INET6 ? kIndexV6 : kIndexV4);
    return static_cast<int>(sent);
}

void QuicEngineCore::enable_write_watch(int index) {
    if (write_watch_[index] || sockets_[index].fd < 0) return;
    if (loop_->mod_fd(sockets_[index].fd, Ready::readable | Ready::writable))
        write_watch_[index] = true;
}

void QuicEngineCore::disable_write_watch(int index) {
    if (sockets_[index].fd >= 0) loop_->mod_fd(sockets_[index].fd, Ready::readable);
}

void QuicEngineCore::schedule_tick() {
    if (!engine_) return;
    int diff = 0;
    unsigned next = lsquic_engine_earliest_adv_tick(engine_, &diff);
    // diff is in microseconds (lsquic_time_t); convert and clamp to 500 ms.
    int ms;
    if (diff < 0) ms = 0;
    else if (next == 0 || static_cast<unsigned>(diff) > 500000) ms = 500;
    else ms = static_cast<int>((static_cast<unsigned>(diff) + 999) / 1000);
    // Re-arming displaces the pending wait with operation_canceled (ITimer
    // contract), which the handler below ignores.
    tick_timer_->expires_after(std::chrono::milliseconds(ms));
    tick_timer_->async_wait([this](const std::error_code& ec) {
        if (ec || !engine_) return;
        lsquic_engine_process_conns(engine_);
        schedule_tick();
    });
}

} // namespace pr::net::quic
