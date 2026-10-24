#ifndef PR_FRAMEWORK_NET_NATIVE_STREAM_INTERNAL_H
#define PR_FRAMEWORK_NET_NATIVE_STREAM_INTERNAL_H
// Shared internals of the native TCP transports: the loop-side state machine
// for one nonblocking socket. Not part of any public interface.
#include "framework/net/native_runtime.h"

#include <atomic>
#include <string>
#include <sys/socket.h>

namespace pr::net::native {

// One TCP stream's operation state. Everything except `open` is confined to
// the owning loop thread (initiations from foreign threads are routed through
// loop->post); completions always go through the loop's task queue, so they
// are serialized with every other callback on the executor and never run
// inline at the initiation site.
struct StreamState : std::enable_shared_from_this<StreamState> {
    StreamState(NativeLoopPtr owner, int socket_fd);

    // ---- initiations (loop thread; wrappers route foreign callers) ----
    void initiate_read(net::MutableBuffer buffer, IConnection::ReadHandler handler);
    void initiate_write(const void* data, std::size_t size, IConnection::WriteHandler handler);
    // Client connect, second half: the caller (client transport) has claimed
    // `connecting` and resolved `address`; this creates the socket and starts
    // the nonblocking connect.
    void begin_socket_connect(const sockaddr_storage& address, socklen_t length);
    void initiate_cancel();
    void initiate_close();
    // Resolution-failure path for the detached resolver thread.
    void complete_connect_public(const std::error_code& error) { complete_connect(error); }

    // ---- loop callbacks ----
    void handle_event(unsigned ready);
    void handle_loop_stopping();

    // ---- teardown (runs on the loop thread via defer_destroy) ----
    void dispose();

    bool is_open() const { return fd >= 0 && open.load(std::memory_order_acquire); }

    NativeLoopPtr loop;
    int fd = -1; // -1 once the socket is closed
    std::atomic<bool> open{true};

    // loop-thread-only operation state; at most one of each may be pending.
    bool read_pending = false;
    net::MutableBuffer read_buffer{};
    IConnection::ReadHandler read_handler;

    bool write_pending = false;
    std::string write_data; // copy: completion never depends on caller storage
    std::size_t write_offset = 0;
    IConnection::WriteHandler write_handler;

    bool connecting = false;
    IConnection::HandshakeHandler connect_handler;

    // endpoint cache, filled at construction/establishment, then immutable
    uint16_t local_port = 0;
    std::string remote_ip;
    uint16_t remote_port = 0;

    std::shared_ptr<LoopEntry> entry;
    unsigned interest = 0;
    bool registered = false;

private:
    void ensure_entry(); // builds the LoopEntry once shared ownership exists
    void do_read();
    void do_flush();
    void finish_connect();
    void cache_endpoints();
    void complete_read(const std::error_code& error, std::size_t bytes);
    void complete_write(const std::error_code& error, std::size_t bytes);
    void complete_connect(const std::error_code& error);
    void cancel_pending(const std::error_code& error);
    void update_interest();
};

// IConnection surface shared by the accepted-connection and client transports.
class NativeStreamBase : public IConnection {
public:
    std::shared_ptr<net::IExecutor> executor() override { return state_->loop; }

    void async_read(net::MutableBuffer buffer, ReadHandler handler) override {
        run_on_loop([state = state_, buffer, handler = std::move(handler)]() mutable {
            state->initiate_read(buffer, std::move(handler));
        });
    }

    void async_write(net::ConstBuffer buffer, WriteHandler handler) override {
        run_on_loop([state = state_, buffer, handler = std::move(handler)]() mutable {
            state->initiate_write(buffer.data(), buffer.size(), std::move(handler));
        });
    }

    void async_handshake(net::HandshakeRole, HandshakeHandler handler) override {
        // Plain TCP needs no handshake; still deliver asynchronously.
        state_->loop->post([handler = std::move(handler)] { handler(std::error_code{}); });
    }

    void close() override {
        state_->open.store(false, std::memory_order_release);
        run_on_loop([state = state_] { state->initiate_close(); });
    }

    void cancel() override {
        run_on_loop([state = state_] { state->initiate_cancel(); });
    }

    bool is_open() const override { return state_->open.load(std::memory_order_acquire); }
    uint16_t get_local_port() const override { return state_->local_port; }
    std::string get_remote_ip() const override { return state_->remote_ip; }
    int native_handle() const override { return state_->fd; }

    // Consumes the stream out of this wrapper (used by the TLS upgrade: the
    // socket, its loop registration and the serialized executor survive while
    // the wrapper itself is discarded). The wrapper must not be used again;
    // its destructor then owns nothing.
    std::shared_ptr<StreamState> release_state() { return std::move(state_); }

    ~NativeStreamBase() override;

protected:
    explicit NativeStreamBase(std::shared_ptr<StreamState> state) : state_(std::move(state)) {}

    void run_on_loop(std::function<void()> action);

    std::shared_ptr<StreamState> state_;
};

// Wraps an already-accepted socket (listener path).
std::unique_ptr<IConnection> make_native_tcp_connection(int fd, const NativeLoopPtr& loop);

} // namespace pr::net::native
#endif
