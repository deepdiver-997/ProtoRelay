// Native client connect factory: IClientTransport over the native runtime.
// v1 is plain TCP; async_start_tls refuses (no TLS in this backend yet).
#include "framework/net/native_transport.h"
#include "native/native_stream_internal.h"

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
public:
    explicit NativeClientTransport(std::shared_ptr<NativeLoop> loop) : loop_(std::move(loop)) {
        if (!loop_) throw std::invalid_argument("client transport requires an executor");
        state_ = std::make_shared<StreamState>(loop_, -1);
    }

    std::shared_ptr<net::IExecutor> executor() override { return loop_; }

    void async_connect(const std::string& host, uint16_t port,
                       HandshakeHandler handler) override {
        const auto initiate = [state = state_, host, port,
                               handler = std::move(handler)]() mutable {
            state->loop->assert_loop_thread("async_connect");
            if (state->connecting) throw std::logic_error("connect already pending");
            if (state->fd >= 0) state->initiate_close(); // reconnect semantics
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
        // TLS is a backend capability; the native backend v1 has none.
        const bool invalid = config.server_name.empty();
        loop_->post([handler = std::move(handler), invalid] {
            handler(std::make_error_code(invalid ? std::errc::invalid_argument
                                                 : std::errc::operation_not_supported));
        });
    }

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

    void async_handshake(net::HandshakeRole role, HandshakeHandler handler) override {
        // async_start_tls performs the only TLS handshake; an established
        // client connection needs no second handshake (Asio parity).
        const bool connected = state_->is_open();
        loop_->post([role, connected, handler = std::move(handler)] {
            handler(!connected ? std::make_error_code(std::errc::not_connected) :
                    role == net::HandshakeRole::client ? std::error_code{} :
                    std::make_error_code(std::errc::operation_not_supported));
        });
    }

    void close() override {
        state_->open.store(false, std::memory_order_release);
        run_on_loop([state = state_] { state->initiate_close(); });
    }

    void cancel() override {
        run_on_loop([state = state_] { state->initiate_cancel(); });
    }

    bool is_open() const override { return state_->is_open(); }
    uint16_t get_local_port() const override { return state_->local_port; }
    std::string get_remote_ip() const override { return state_->remote_ip; }
    int native_handle() const override { return state_->fd; }

    ~NativeClientTransport() override {
        auto state = std::move(state_);
        state->open.store(false, std::memory_order_release);
        loop_->defer_destroy([state] { state->dispose(); });
    }

private:
    void run_on_loop(std::function<void()> action) {
        if (loop_->is_loop_thread()) action();
        else loop_->post(std::move(action));
    }

    std::shared_ptr<NativeLoop> loop_;
    std::shared_ptr<StreamState> state_;
};
} // namespace

std::shared_ptr<net::IClientTransport> make_native_client_transport(
    std::shared_ptr<net::IExecutor> executor) {
    const auto loop = std::dynamic_pointer_cast<NativeLoop>(std::move(executor));
    if (!loop) throw std::invalid_argument("native client transport requires a native loop executor");
    return std::make_shared<NativeClientTransport>(loop);
}

} // namespace pr::net::native
