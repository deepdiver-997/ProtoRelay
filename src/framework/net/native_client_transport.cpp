// Native client connect factory: IClientTransport over the native runtime.
// Plain TCP always works; async_start_tls needs the optional
// protorelay_native_tls module (enable_native_tls) — without it the call
// fails closed with operation_not_supported. After STARTTLS every IO routes
// through the TLS connection wrapping this transport's stream.
#include "framework/net/native_transport.h"
#include "native/native_stream_internal.h"
#include "native/native_tls_hook.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <stdexcept>
#include <thread>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace pr::net::native {
namespace {
bool parse_numeric(const std::string& host, uint16_t port, sockaddr_storage& storage,
                   socklen_t& length) {
    std::memset(&storage, 0, sizeof(storage));
    sockaddr_in* address4 = reinterpret_cast<sockaddr_in*>(&storage);
    if (::inet_pton(AF_INET, host.c_str(), &address4->sin_addr) == 1) {
        address4->sin_family = AF_INET;
        address4->sin_port = htons(port);
        length = sizeof(sockaddr_in);
        return true;
    }
    sockaddr_in6* address6 = reinterpret_cast<sockaddr_in6*>(&storage);
    if (::inet_pton(AF_INET6, host.c_str(), &address6->sin6_addr) == 1) {
        address6->sin6_family = AF_INET6;
        address6->sin6_port = htons(port);
        length = sizeof(sockaddr_in6);
        return true;
    }
    return false;
}

// Blocking resolution runs on a detached thread so the event loop never
// blocks; numeric literals (the common local/test case) skip the thread.
bool resolve_blocking(const std::string& host, uint16_t port, sockaddr_storage& storage,
                      socklen_t& length) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &results);
    if (rc != 0 || !results) return false;
    std::memcpy(&storage, results->ai_addr, std::min<size_t>(results->ai_addrlen, sizeof(storage)));
    length = results->ai_addrlen;
    ::freeaddrinfo(results);
    return true;
}

class NativeClientTransport final : public net::IClientTransport {
    // Post-STARTTLS slot. Mutations happen on the loop thread, but close(),
    // cancel() and destruction may run on any thread, and loop tasks must
    // never capture `this` — so closures capture this holder instead and copy
    // the connection out under the mutex.
    struct TlsSlot {
        mutable std::mutex mutex;
        std::shared_ptr<IConnection> connection;
        std::string diagnostic; // setup failures before a connection exists
        std::atomic<bool> active{false}; // raw fd must hide once TLS is on

        std::shared_ptr<IConnection> get() const {
            std::lock_guard<std::mutex> lock(mutex);
            return connection;
        }
        void set(std::shared_ptr<IConnection> value) {
            std::lock_guard<std::mutex> lock(mutex);
            connection = std::move(value);
        }
        std::shared_ptr<IConnection> clear() {
            std::lock_guard<std::mutex> lock(mutex);
            return std::move(connection);
        }
        void set_diagnostic(std::string text) {
            std::lock_guard<std::mutex> lock(mutex);
            diagnostic = std::move(text);
        }
        std::string get_diagnostic(const IConnection* connection) const {
            std::lock_guard<std::mutex> lock(mutex);
            return connection ? connection->get_handshake_diagnostic() : diagnostic;
        }
    };

public:
    explicit NativeClientTransport(std::shared_ptr<NativeLoop> loop) : loop_(std::move(loop)) {
        if (!loop_) throw std::invalid_argument("client transport requires an executor");
        state_ = std::make_shared<StreamState>(loop_, -1);
        tls_ = std::make_shared<TlsSlot>();
    }

    std::shared_ptr<net::IExecutor> executor() override { return loop_; }

    void async_connect(const std::string& host, uint16_t port,
                       HandshakeHandler handler) override {
        const auto initiate = [state = state_, slot = tls_, host, port,
                               handler = std::move(handler)]() mutable {
            state->loop->assert_loop_thread("async_connect");
            if (state->connecting) throw std::logic_error("connect already pending");
            if (state->fd >= 0) state->initiate_close(); // reconnect semantics
            // A reconnect starts plain again (Asio parity: fresh state per
            // connect); the abandoned TLS session is disposed, silently.
            if (auto stale = slot->clear()) slot->active.store(false, std::memory_order_release);
            state->connecting = true;
            state->connect_handler = std::move(handler);
            sockaddr_storage storage{};
            socklen_t length = 0;
            if (parse_numeric(host, port, storage, length)) {
                state->begin_socket_connect(storage, length);
                return;
            }
            const std::weak_ptr<StreamState> weak = state;
            std::thread([weak, host, port] {
                sockaddr_storage storage{};
                socklen_t length = 0;
                const bool resolved = resolve_blocking(host, port, storage, length);
                // Re-enter the loop with the outcome; a canceled connect
                // (connecting flag cleared) is dropped there.
                if (const auto state = weak.lock()) {
                    state->loop->post([state, resolved, storage, length] {
                        if (!state->connecting) return; // canceled while resolving
                        if (!resolved) {
                            state->connecting = false;
                            state->complete_connect_public(
                                std::make_error_code(std::errc::host_unreachable));
                            return;
                        }
                        state->begin_socket_connect(storage, length);
                    });
                }
            }).detach();
        };
        run_on_loop(std::move(initiate));
    }

    void async_start_tls(ClientTlsConfig config, HandshakeHandler handler) override {
        loop_->post([state = state_, slot = tls_, handler = std::move(handler),
                     config = std::move(config)]() mutable {
            begin_start_tls(state, slot, std::move(config), std::move(handler));
        });
    }

    void async_read(net::MutableBuffer buffer, ReadHandler handler) override {
        run_on_loop([state = state_, slot = tls_, buffer, handler = std::move(handler)]() mutable {
            if (auto connection = slot->get())
                connection->async_read(buffer, std::move(handler));
            else
                state->initiate_read(buffer, std::move(handler));
        });
    }

    void async_write(net::ConstBuffer buffer, WriteHandler handler) override {
        run_on_loop([state = state_, slot = tls_, buffer, handler = std::move(handler)]() mutable {
            if (auto connection = slot->get())
                connection->async_write(buffer, std::move(handler));
            else
                state->initiate_write(buffer.data(), buffer.size(), std::move(handler));
        });
    }

    void async_handshake(net::HandshakeRole role, HandshakeHandler handler) override {
        // Without TLS the established plain connection needs no handshake
        // (Asio parity); with TLS the call drives the handshake on it.
        run_on_loop([state = state_, slot = tls_, role, handler = std::move(handler)]() mutable {
            if (auto connection = slot->get()) {
                connection->async_handshake(role, std::move(handler));
                return;
            }
            const bool connected = state->is_open();
            state->loop->post([role, connected, handler = std::move(handler)] {
                handler(!connected ? std::make_error_code(std::errc::not_connected) :
                        role == net::HandshakeRole::client ? std::error_code{} :
                        std::make_error_code(std::errc::operation_not_supported));
            });
        });
    }

    void close() override {
        state_->open.store(false, std::memory_order_release);
        run_on_loop([state = state_, slot = tls_] {
            if (auto connection = slot->get())
                connection->close(); // routes close_notify, then the socket
            else
                state->initiate_close();
        });
    }

    void cancel() override {
        run_on_loop([state = state_, slot = tls_] {
            if (auto connection = slot->get())
                connection->cancel();
            else
                state->initiate_cancel();
        });
    }

    bool is_open() const override { return state_->is_open(); }
    uint16_t get_local_port() const override { return state_->local_port; }
    std::string get_remote_ip() const override { return state_->remote_ip; }
    int native_handle() const override {
        // After STARTTLS the raw fd would bypass the TLS layer — hidden.
        return tls_->active.load(std::memory_order_acquire) ? -1 : state_->fd;
    }
    std::string get_handshake_diagnostic() const override {
        return tls_->get_diagnostic(tls_->get().get());
    }

    ~NativeClientTransport() override {
        auto tls = tls_->clear();
        auto state = std::move(state_);
        state->open.store(false, std::memory_order_release);
        // Graves FIFO: the TLS teardown (SSL_free + stream dispose) runs
        // before the plain dispose, which stays for the non-TLS case and is
        // idempotent when TLS already closed everything.
        loop_->defer_destroy([tls = std::move(tls)] { });
        loop_->defer_destroy([state] { state->dispose(); });
    }

private:
    // Loop thread. On success the transport keeps serving through the TLS
    // connection wrapped around this transport's own stream state.
    static void begin_start_tls(const std::shared_ptr<StreamState>& state,
                                const std::shared_ptr<TlsSlot>& slot, ClientTlsConfig config,
                                HandshakeHandler handler) {
        state->loop->assert_loop_thread("async_start_tls");
        const auto fail = [state, &handler](std::error_code error) {
            state->loop->post([handler = std::move(handler), error] { handler(error); });
        };
        if (config.server_name.empty()) return fail(std::make_error_code(std::errc::invalid_argument));
        auto* hooks = tls_hook::get_hooks();
        if (!hooks)
            return fail(std::make_error_code(std::errc::operation_not_supported));
        if (!state->is_open() || state->connecting)
            return fail(std::make_error_code(std::errc::not_connected));
        if (slot->get()) return fail(std::make_error_code(std::errc::invalid_argument));
        if (state->read_pending || state->write_pending)
            throw std::logic_error("async_start_tls requires an idle transport");
        std::shared_ptr<IConnection> connection;
        try {
            const auto context = hooks->make_client_context(config);
            connection = hooks->wrap(state->loop, state, context, net::HandshakeRole::client);
        } catch (const std::exception& error) {
            slot->set_diagnostic(error.what());
            return fail(std::make_error_code(std::errc::protocol_error));
        }
        slot->set(connection);
        slot->active.store(true, std::memory_order_release);
        // Already on the loop thread: this arms the handshake; its completion
        // is queued like every other callback.
        connection->async_handshake(net::HandshakeRole::client, std::move(handler));
    }

    void run_on_loop(std::function<void()> action) {
        if (loop_->is_loop_thread()) action();
        else loop_->post(std::move(action));
    }

    std::shared_ptr<NativeLoop> loop_;
    std::shared_ptr<StreamState> state_;
    std::shared_ptr<TlsSlot> tls_;
};
} // namespace

std::shared_ptr<net::IClientTransport> make_native_client_transport(
    std::shared_ptr<net::IExecutor> executor) {
    const auto loop = std::dynamic_pointer_cast<NativeLoop>(std::move(executor));
    if (!loop) throw std::invalid_argument("native client transport requires a native loop executor");
    return std::make_shared<NativeClientTransport>(loop);
}

} // namespace pr::net::native
