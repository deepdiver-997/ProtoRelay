#include "quic_transport_impl.h"

#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/uio.h>

namespace pr::net::quic {

namespace {
// Completion posters: handlers never run inline (IConnection discipline),
// always through the loop's task queue. `loop` rides along in every task so
// the executor outlives the wrapper that initiated the operation.
void post_read(const std::shared_ptr<NativeLoop>& loop, IConnection::ReadHandler handler,
               std::error_code error, size_t bytes) {
    loop->post([loop, handler = std::move(handler), error, bytes] { handler(error, bytes); });
}
void post_write(const std::shared_ptr<NativeLoop>& loop, IConnection::WriteHandler handler,
                std::error_code error, size_t bytes) {
    loop->post([loop, handler = std::move(handler), error, bytes] { handler(error, bytes); });
}
void post_handshake(const std::shared_ptr<NativeLoop>& loop, IConnection::HandshakeHandler handler,
                    std::error_code error) {
    loop->post([loop, handler = std::move(handler), error] { handler(error); });
}
} // namespace

QuicTransportBase::~QuicTransportBase() = default;

// ── QuicStream ──

QuicStream::QuicStream(QuicMuxConnection* owner, std::shared_ptr<QuicEngineCore> core,
                       lsquic_stream_t* stream)
    : loop_(core->loop_ptr()),
      core_(std::move(core)),
      owner_(owner),
      stream_(stream),
      local_port_(owner->local_port()),
      remote_ip_(owner->remote_ip()) {
    owner_->register_stream(this);
    // Latched for the stream's whole life: lsquic buffers arriving data
    // until we read it. Clearing wantread while data is in flight makes
    // lsquic send STOP_SENDING and kill the stream.
    lsquic_stream_wantread(stream_, 1);
}

QuicStream::~QuicStream() {
    lsquic_stream_t* stream = stream_;
    stream_ = nullptr;
    if (owner_) owner_->unregister_stream(this);
    if (stream) {
        // Destruction without close(): detach the ctx first (lsquic still
        // fires on_close with a null handle), then finish the stream so the
        // peer sees the FIN.
        run_on_loop(*loop_, [loop = loop_, stream] {
            lsquic_stream_set_ctx(stream, nullptr);
            lsquic_stream_flush(stream);
            lsquic_stream_close(stream);
        });
    }
    // Safety net: destruction with ops still pending completes them once.
    if (read_handler_) {
        auto handler = std::move(read_handler_);
        post_read(loop_, std::move(handler), std::make_error_code(std::errc::operation_canceled), 0);
    }
    if (write_handler_) {
        auto handler = std::move(write_handler_);
        post_write(loop_, std::move(handler), std::make_error_code(std::errc::operation_canceled), 0);
    }
}

void QuicStream::async_read(MutableBuffer buffer, ReadHandler handler) {
    if (!stream_) {
        // Connection-level end or local close: reads are over.
        post_read(loop_, std::move(handler), make_error_code(IoErrc::end_of_stream), 0);
        return;
    }
    read_buffer_ = buffer;
    read_handler_ = std::move(handler);
    // Read immediately: data buffered inside lsquic (arrived between reads)
    // must complete this call. The handler itself is posted — never inline.
    try_read();
}

void QuicStream::try_read() {
    auto handler = std::move(read_handler_);
    read_handler_ = nullptr;
    if (!handler) return;
    if (!stream_) {
        post_read(loop_, std::move(handler), make_error_code(IoErrc::end_of_stream), 0);
        return;
    }
    iovec vec{read_buffer_.data(), read_buffer_.size()};
    ssize_t n = lsquic_stream_readv(stream_, &vec, 1);
    if (n > 0) {
        post_read(loop_, std::move(handler), {}, static_cast<size_t>(n));
    } else if (n == 0) {
        // EOF: the peer finished; stop read callbacks.
        lsquic_stream_wantread(stream_, 0);
        post_read(loop_, std::move(handler), make_error_code(IoErrc::end_of_stream), 0);
    } else if (errno == EWOULDBLOCK || errno == EAGAIN) {
        // No data yet: stay pending; on_readable() retries when it arrives.
        read_handler_ = std::move(handler);
    } else {
        std::error_code error = errno == ECONNRESET
                                    ? std::make_error_code(std::errc::connection_reset)
                                    : make_error_code(IoErrc::end_of_stream);
        post_read(loop_, std::move(handler), error, 0);
    }
}

void QuicStream::on_readable() {
    // try_read() nulls the member before completing, so a second readiness
    // callback without a new async_read is a no-op.
    if (read_handler_) try_read();
}


void QuicStream::async_write(ConstBuffer buffer, WriteHandler handler) {
    if (!stream_) {
        post_write(loop_, std::move(handler), std::make_error_code(std::errc::connection_reset), 0);
        return;
    }
    write_buffer_ = buffer;
    write_offset_ = 0;
    write_handler_ = std::move(handler);
    // Completion arrives from on_writeable() (loop event phase), never inline.
    lsquic_stream_wantwrite(stream_, 1);
}

void QuicStream::pump_write() {
    auto handler = std::move(write_handler_);
    write_handler_ = nullptr;
    if (!handler) return;
    if (!stream_) {
        post_write(loop_, std::move(handler), std::make_error_code(std::errc::connection_reset), 0);
        return;
    }
    const char* base = static_cast<const char*>(write_buffer_.data()) + write_offset_;
    size_t remaining = write_buffer_.size() - write_offset_;
    iovec vec{const_cast<void*>(static_cast<const void*>(base)), remaining};
    ssize_t n = lsquic_stream_writev(stream_, &vec, 1);
    if (n > 0) {
        write_offset_ += static_cast<size_t>(n);
        if (write_offset_ >= write_buffer_.size()) {
            // Fully accepted: flush the buffered bytes into the packetizer
            // (writev alone only buffers), then stop write callbacks —
            // leaving wantwrite set makes lsquic spin on_write.
            lsquic_stream_flush(stream_);
            lsquic_stream_wantwrite(stream_, 0);
            post_write(loop_, std::move(handler), {}, write_buffer_.size());
            return;
        }
    } else if (n < 0 && errno != EWOULDBLOCK && errno != EAGAIN) {
        std::error_code error = errno == ECONNRESET ? std::make_error_code(std::errc::connection_reset)
                                                    : std::make_error_code(std::errc::broken_pipe);
        post_write(loop_, std::move(handler), error, 0);
        return;
    }
    // Partial or nothing accepted: keep pending, resume on on_writeable().
    write_handler_ = std::move(handler);
    lsquic_stream_wantwrite(stream_, 1);
}

void QuicStream::on_writeable() {
    if (write_handler_) pump_write();
}

void QuicStream::async_handshake(HandshakeRole role, HandshakeHandler handler) {
    // QUIC handshakes are connection-level; a live stream always "handshakes"
    // instantly (the plain-transport IConnection discipline).
    post_handshake(loop_, std::move(handler), {});
}

void QuicStream::close() {
    if (!stream_ || closing_) return;
    closing_ = true;
    lsquic_stream_flush(stream_);
    lsquic_stream_close(stream_); // FIN queued; on_close fires once drained
    // Pending ops end now, exactly once, on the executor. Handler members
    // are nulled explicitly (libc++ moved-from functions stay non-empty).
    if (read_handler_) {
        auto handler = std::move(read_handler_);
        read_handler_ = nullptr;
        post_read(loop_, std::move(handler), make_error_code(IoErrc::end_of_stream), 0);
    }
    if (write_handler_) {
        auto handler = std::move(write_handler_);
        write_handler_ = nullptr;
        post_write(loop_, std::move(handler), std::make_error_code(std::errc::operation_canceled), 0);
    }
}

void QuicStream::cancel() {
    // Cancel is not close: a displaced read keeps the stream open; buffered
    // data stays until the next async_read.
    if (read_handler_) {
        auto handler = std::move(read_handler_);
        post_read(loop_, std::move(handler), std::make_error_code(std::errc::operation_canceled), 0);
    }
}

void QuicStream::on_reset(int how) {
    // Peer reset one or both directions; unblock pending ops (on_close
    // follows later).
    if (how == 0 || how == 2) {
        if (read_handler_) {
            auto handler = std::move(read_handler_);
            post_read(loop_, std::move(handler), std::make_error_code(std::errc::connection_reset), 0);
        }
    }
    if (how == 1 || how == 2) {
        if (write_handler_) {
            auto handler = std::move(write_handler_);
            post_write(loop_, std::move(handler), std::make_error_code(std::errc::connection_reset), 0);
        }
    }
}

void QuicStream::on_closed() {
    // Last lsquic callback for this stream.
    stream_ = nullptr;
    closing_ = true;
    if (owner_) owner_->unregister_stream(this);
    if (read_handler_) {
        auto handler = std::move(read_handler_);
        post_read(loop_, std::move(handler), make_error_code(IoErrc::end_of_stream), 0);
    }
    if (write_handler_) {
        auto handler = std::move(write_handler_);
        post_write(loop_, std::move(handler), std::make_error_code(std::errc::broken_pipe), 0);
    }
}

void QuicStream::on_conn_gone() {
    // Connection-level end: reads end with end_of_stream, writes fail.
    if (!stream_) return;
    stream_ = nullptr;
    if (read_handler_) {
        auto handler = std::move(read_handler_);
        post_read(loop_, std::move(handler), make_error_code(IoErrc::end_of_stream), 0);
    }
    if (write_handler_) {
        auto handler = std::move(write_handler_);
        post_write(loop_, std::move(handler), std::make_error_code(std::errc::connection_reset), 0);
    }
}

} // namespace pr::net::quic
