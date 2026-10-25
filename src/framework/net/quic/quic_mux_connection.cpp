#include "quic_transport_impl.h"

#include <arpa/inet.h>
#include <cassert>
#include <netinet/in.h>

namespace pr::net::quic {

namespace {
// Stream-event completions ride the loop like every other handler; the box
// carries the move-only stream through the copyable task.
void post_stream(const std::shared_ptr<NativeLoop>& loop, IMuxConnection::StreamHandler handler,
                 std::error_code error, std::unique_ptr<IConnection> stream) {
    auto box = std::make_shared<std::unique_ptr<IConnection>>(std::move(stream));
    loop->post([loop, handler = std::move(handler), error, box]() mutable {
        handler(error, std::move(*box));
    });
}
void post_stream_error(const std::shared_ptr<NativeLoop>& loop, IMuxConnection::StreamHandler handler,
                       std::error_code error) {
    post_stream(loop, std::move(handler), error, nullptr);
}
} // namespace

QuicMuxConnection::QuicMuxConnection(std::shared_ptr<QuicEngineCore> core, lsquic_conn_t* conn,
                                     bool is_client)
    : core_(std::move(core)), conn_(conn), is_client_(is_client) {
    // Cache the endpoints once: streams and diagnostics read them back after
    // the lsquic connection is gone.
    const struct sockaddr* local = nullptr;
    const struct sockaddr* peer = nullptr;
    lsquic_conn_get_sockaddr(conn_, &local, &peer);
    if (local && local->sa_family == AF_INET)
        local_port_ = ntohs(reinterpret_cast<const sockaddr_in*>(local)->sin_port);
    else if (local)
        local_port_ = ntohs(reinterpret_cast<const sockaddr_in6*>(local)->sin6_port);
    if (peer) {
        char buf[INET6_ADDRSTRLEN] = {};
        if (peer->sa_family == AF_INET)
            inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(peer)->sin_addr, buf, sizeof(buf));
        else
            inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(peer)->sin6_addr, buf, sizeof(buf));
        remote_ip_ = buf;
    }
    lsquic_conn_set_ctx(conn_, reinterpret_cast<lsquic_conn_ctx_t*>(this));
}

QuicMuxConnection::~QuicMuxConnection() {
    auto loop = core_->loop_ptr();
    // Displace pendings once — the application dropped the object.
    // NOTE: libc++ leaves a moved-from std::function non-empty when the
    // target fits its small buffer, so handler members are always nulled
    // explicitly after being moved out — the non-null checks below are the
    // exactly-once guards.
    if (open_handler_) {
        auto handler = std::move(open_handler_);
        open_handler_ = nullptr;
        post_stream_error(loop, std::move(handler), std::make_error_code(std::errc::operation_canceled));
    }
    if (accept_handler_) {
        auto handler = std::move(accept_handler_);
        accept_handler_ = nullptr;
        post_stream_error(loop, std::move(handler), std::make_error_code(std::errc::operation_canceled));
    }
    open_pending_ = false;
    for (auto* stream : live_streams_) {
        stream->detach_owner();
        stream->on_conn_gone();
    }
    live_streams_.clear();
    accepted_.clear(); // wrappers detach from lsquic in their own destructor
    if (lsquic_conn_t* conn = conn_) {
        conn_ = nullptr;
        auto core = core_;
        run_on_loop(*loop, [core, conn] {
            lsquic_conn_set_ctx(conn, nullptr);
            lsquic_conn_close(conn); // on_conn_closed fires with a null ctx
        });
    }
}

void QuicMuxConnection::open_stream(StreamHandler handler) {
    if (!conn_) {
        post_stream_error(core_->loop_ptr(), std::move(handler), std::make_error_code(std::errc::operation_canceled));
        return;
    }
    assert(!open_pending_); // at most one pending open (contract discipline)
    open_pending_ = true;
    open_handler_ = std::move(handler);
    // Delivered via on_new_stream (never inline); refused streams arrive as
    // a NULL stream notification.
    lsquic_conn_make_stream(conn_);
}

void QuicMuxConnection::accept_stream(StreamHandler handler) {
    if (!conn_) {
        post_stream_error(core_->loop_ptr(), std::move(handler), std::make_error_code(std::errc::operation_canceled));
        return;
    }
    if (!accepted_.empty()) {
        auto stream = std::move(accepted_.front());
        accepted_.pop_front();
        post_stream(core_->loop_ptr(), std::move(handler), {}, std::move(stream));
        return;
    }
    // Suspends until the peer opens a stream; peer streams opened meanwhile
    // queue in accepted_ (contract discipline: buffered, never refused).
    accept_handler_ = std::move(handler);
}

QuicStream* QuicMuxConnection::on_new_stream(lsquic_stream_t* stream) {
    if (!conn_) return nullptr; // connection already torn down in the wrapper
    auto wrapper = std::make_unique<QuicStream>(this, core_, stream);
    QuicStream* raw = wrapper.get();
    if (locally_initiated(stream)) {
        open_pending_ = false;
        auto handler = std::move(open_handler_);
        open_handler_ = nullptr;
        post_stream(core_->loop_ptr(), std::move(handler), {}, std::move(wrapper));
    } else if (accept_handler_) {
        auto handler = std::move(accept_handler_);
        accept_handler_ = nullptr;
        post_stream(core_->loop_ptr(), std::move(handler), {}, std::move(wrapper));
    } else {
        accepted_.push_back(std::move(wrapper));
    }
    return raw;
}

void QuicMuxConnection::on_hsk_done(lsquic_hsk_status status) {
    if (auto* transport = core_->transport()) transport->on_hsk_done(conn_, status);
}

void QuicMuxConnection::close() {
    if (!conn_ || closed_locally_) return;
    closed_locally_ = true;
    close_reason_ = {std::error_code{}, "local close"};
    // Pendings end now with operation_canceled (local shutdown); the queued
    // pending stream make is withdrawn so no NULL notification arrives.
    if (open_handler_) {
        if (open_pending_) lsquic_conn_cancel_pending_streams(conn_, 1);
        open_pending_ = false;
        auto handler = std::move(open_handler_);
        open_handler_ = nullptr;
        post_stream_error(core_->loop_ptr(), std::move(handler), std::make_error_code(std::errc::operation_canceled));
    }
    if (accept_handler_) {
        auto handler = std::move(accept_handler_);
        accept_handler_ = nullptr;
        post_stream_error(core_->loop_ptr(), std::move(handler), std::make_error_code(std::errc::operation_canceled));
    }
    // CONNECTION_CLOSE goes out with the next process_conns tick.
    lsquic_conn_close(conn_);
    core_->kick();
}

void QuicMuxConnection::cancel() {
    // Cancel is not close: only the suspended mux-level operations go away.
    if (open_handler_) {
        if (open_pending_ && conn_) lsquic_conn_cancel_pending_streams(conn_, 1);
        open_pending_ = false;
        auto handler = std::move(open_handler_);
        open_handler_ = nullptr;
        post_stream_error(core_->loop_ptr(), std::move(handler), std::make_error_code(std::errc::operation_canceled));
    }
    if (accept_handler_) {
        auto handler = std::move(accept_handler_);
        accept_handler_ = nullptr;
        post_stream_error(core_->loop_ptr(), std::move(handler), std::make_error_code(std::errc::operation_canceled));
    }
}

void QuicMuxConnection::on_conn_closed() {
    if (!conn_) return;
    if (!closed_locally_) {
        char errbuf[200] = {};
        int status = lsquic_conn_status(conn_, errbuf, sizeof(errbuf));
        close_reason_ = map_conn_status(status, errbuf, /*local_close=*/false);
    }
    lsquic_conn_set_ctx(conn_, nullptr); // lsquic frees the conn after this
    // The transport erases its pending wrapper table — that would destroy
    // this very object mid-method, so the notification is deferred past the
    // callback (delivered wrappers are simply not in the table).
    if (auto* transport = core_->transport()) {
        lsquic_conn_t* conn = conn_;
        auto core = core_;
        core->loop_ptr()->post([core, transport, conn] { transport->on_conn_closed(conn); });
    }
    conn_ = nullptr;
    for (auto* stream : live_streams_) {
        stream->detach_owner();
        stream->on_conn_gone();
    }
    live_streams_.clear();
    accepted_.clear();
    // Pendings that survived close() (peer-initiated end) end with the
    // connection-level code.
    if (open_handler_) {
        auto handler = std::move(open_handler_);
        open_handler_ = nullptr;
        post_stream_error(core_->loop_ptr(), std::move(handler), close_reason_.code);
    }
    if (accept_handler_) {
        auto handler = std::move(accept_handler_);
        accept_handler_ = nullptr;
        post_stream_error(core_->loop_ptr(), std::move(handler), close_reason_.code);
    }
    open_pending_ = false;
}

bool QuicMuxConnection::locally_initiated(lsquic_stream_t* stream) const {
    // QUIC stream id bit 0: 0 = client-initiated, 1 = server-initiated.
    return (lsquic_stream_id(stream) & 1) == (is_client_ ? 0 : 1);
}

} // namespace pr::net::quic
