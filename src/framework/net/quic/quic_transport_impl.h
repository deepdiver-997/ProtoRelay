#ifndef PR_PROTORELAY_QUIC_TRANSPORT_IMPL_H
#define PR_PROTORELAY_QUIC_TRANSPORT_IMPL_H
// Internal implementations behind the quic_transport.h contracts: the shared
// transport base, the multiplexed-connection wrapper (lsquic_conn_t*) and the
// per-stream IConnection adapter (lsquic_stream_t*).
//
// Threading: all lsquic calls happen on the loop thread. IO initiation is the
// application's contract obligation (on executor()); destruction of delivered
// connections/streams should therefore also happen on the loop thread — from
// foreign threads the detach is marshalled via invoke_when_live, which is
// safe while the loop is live or already gone (the two states a conforming
// application produces), like the native transports' loop-thread discipline.
#include "framework/net/quic_transport.h"
#include "quic_core.h"

#include <deque>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>

extern "C" {
#include <lsquic.h>
}

namespace pr::net::quic {

// Shared by QuicServerTransport / QuicClientTransport: receives the engine
// callbacks that are per-transport rather than per-connection. All entry
// points run on the loop thread.
class QuicMuxConnection;
class QuicStream;

class QuicTransportBase {
public:
    virtual ~QuicTransportBase();
    virtual void on_new_conn(lsquic_conn_t* conn) = 0;
    virtual void on_hsk_done(lsquic_conn_t* conn, lsquic_hsk_status status) = 0;
    virtual void on_conn_closed(lsquic_conn_t* conn) = 0;
};

// IConnection over one lsquic stream. Owned by the application (delivered via
// IMuxConnection handlers). Holds a loop reference for completions and a
// detachable back-pointer to the owning connection wrapper.
class QuicStream final : public IConnection {
public:
    QuicStream(QuicMuxConnection* owner, std::shared_ptr<QuicEngineCore> core,
               lsquic_stream_t* stream);
    ~QuicStream() override;

    QuicStream(const QuicStream&) = delete;
    QuicStream& operator=(const QuicStream&) = delete;

    // IConnection
    std::shared_ptr<IExecutor> executor() override { return loop_; }
    void async_read(MutableBuffer buffer, ReadHandler handler) override;
    void async_write(ConstBuffer buffer, WriteHandler handler) override;
    void async_handshake(HandshakeRole role, HandshakeHandler handler) override;
    void close() override;
    void cancel() override;
    bool is_open() const override { return stream_ != nullptr && !closing_; }
    uint16_t get_local_port() const override { return local_port_; }
    std::string get_remote_ip() const override { return remote_ip_; }

    // lsquic callback entry points (loop thread; h = this).
    void on_readable();
    void on_writeable();
    void on_closed();
    void on_reset(int how);
    // The owning connection died: no further lsquic callbacks for this stream.
    void on_conn_gone();
    // The owning wrapper is going away first: stop referring to it.
    void detach_owner() { owner_ = nullptr; }

private:
    void try_read();
    void pump_write();

    std::shared_ptr<NativeLoop> loop_;
    std::shared_ptr<QuicEngineCore> core_;
    QuicMuxConnection* owner_;
    lsquic_stream_t* stream_;
    uint16_t local_port_;
    std::string remote_ip_;

    ReadHandler read_handler_;
    MutableBuffer read_buffer_{};
    ConstBuffer write_buffer_{};
    size_t write_offset_ = 0;
    WriteHandler write_handler_;
    bool closing_ = false;
};

// IMuxConnection over one lsquic connection. Delivered to the application
// once the handshake completed; detaches from lsquic in its destructor.
class QuicMuxConnection final : public IMuxConnection {
public:
    QuicMuxConnection(std::shared_ptr<QuicEngineCore> core, lsquic_conn_t* conn, bool is_client);
    ~QuicMuxConnection() override;

    QuicMuxConnection(const QuicMuxConnection&) = delete;
    QuicMuxConnection& operator=(const QuicMuxConnection&) = delete;

    // IMuxConnection
    void open_stream(StreamHandler handler) override;
    void accept_stream(StreamHandler handler) override;
    MuxCloseInfo close_reason() const override { return close_reason_; }
    void close() override;
    void cancel() override;
    bool is_open() const override { return conn_ != nullptr; }
    std::shared_ptr<IExecutor> executor() const override { return core_->loop_ptr(); }

    uint16_t local_port() const { return local_port_; }
    const std::string& remote_ip() const { return remote_ip_; }
    const std::shared_ptr<QuicEngineCore>& core() const { return core_; }

    // lsquic callback entry points (loop thread). on_new_stream returns the
    // stream ctx for lsquic (the new QuicStream; peer streams without a
    // parked accept handler are held by `accepted_`).
    QuicStream* on_new_stream(lsquic_stream_t* stream);
    void on_hsk_done(lsquic_hsk_status status);
    void on_conn_closed(); // notifies the transport (no lsquic ctx arg exists)

private:
    friend class QuicStream;
    bool locally_initiated(lsquic_stream_t* stream) const;
    void register_stream(QuicStream* stream) { live_streams_.insert(stream); }
    void unregister_stream(QuicStream* stream) { live_streams_.erase(stream); }

    std::shared_ptr<QuicEngineCore> core_;
    lsquic_conn_t* conn_;
    bool is_client_;
    uint16_t local_port_;
    std::string remote_ip_;

    StreamHandler open_handler_;
    StreamHandler accept_handler_;
    bool open_pending_ = false;
    std::deque<std::unique_ptr<QuicStream>> accepted_;
    std::set<QuicStream*> live_streams_;
    bool closed_locally_ = false;
    MuxCloseInfo close_reason_{};
};

} // namespace pr::net::quic

#endif // PR_PROTORELAY_QUIC_TRANSPORT_IMPL_H
