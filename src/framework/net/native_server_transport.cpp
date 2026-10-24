// Native TCP listener backend: IServerTransport over the native runtime.
// Plain TCP listeners always work. When the optional protorelay_native_tls
// module installed its hooks (see native_tls.h / enable_native_tls), SSL-type
// listeners accept TLS connections, configure_tls loads a server context and
// upgrade_tls wraps a plain connection for STARTTLS. Without the module
// everything TLS fails closed with operation_not_supported.
#include "framework/net/native_transport.h"
#include "native/native_stream_internal.h"
#include "native/native_tls_hook.h"
#include "framework/log/log_macros.h"

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <cstring>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace pr::net::native {
namespace {
std::error_code last_error() { return std::error_code(errno, std::generic_category()); }

// Per-transport TLS context slot: configure_tls may run on an application
// thread while accepts and upgrades happen on loop threads.
struct TlsContextHolder {
    mutable std::mutex mutex;
    tls_hook::Hooks::Context context; // null until a context was configured
    tls_hook::Hooks::Context snapshot() const {
        std::lock_guard<std::mutex> lock(mutex);
        return context;
    }
    void store(tls_hook::Hooks::Context value) {
        std::lock_guard<std::mutex> lock(mutex);
        context = std::move(value);
    }
};

struct ListenerState {
    ListenerConfig config;
    int fd = -1;
    NativeLoopPtr loop;
    std::shared_ptr<LoopEntry> entry;
    bool registered = false;
    net::IServerTransport::AcceptHandler handler; // loop-thread only
    tls_hook::Hooks* hooks = nullptr;             // null without the TLS module
    std::shared_ptr<TlsContextHolder> tls;
};

// Returns the sockaddr length, or 0 when the address is not a numeric IPv4/
// IPv6 literal. An empty string binds the IPv4 wildcard.
socklen_t parse_bind_address(const std::string& text, sockaddr_storage& storage) {
    sockaddr_in* address4 = reinterpret_cast<sockaddr_in*>(&storage);
    std::memset(&storage, 0, sizeof(storage));
    if (text.empty()) {
        address4->sin_family = AF_INET;
        return sizeof(sockaddr_in);
    }
    if (::inet_pton(AF_INET, text.c_str(), &address4->sin_addr) == 1) {
        address4->sin_family = AF_INET;
        return sizeof(sockaddr_in);
    }
    sockaddr_in6* address6 = reinterpret_cast<sockaddr_in6*>(&storage);
    if (::inet_pton(AF_INET6, text.c_str(), &address6->sin6_addr) == 1) {
        address6->sin6_family = AF_INET6;
        return sizeof(sockaddr_in6);
    }
    return 0;
}

void open_listen_socket(ListenerState& state, const sockaddr_storage& address,
                        socklen_t address_length) {
    const int fd = ::socket(address.ss_family, SOCK_STREAM, 0);
    if (fd < 0) throw std::system_error(last_error(), "native listener socket");
    // The accept drain loop keeps accepting until EAGAIN: a blocking listener
    // socket would park the whole event loop on the second call.
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    const int fd_flags = ::fcntl(fd, F_GETFD, 0);
    if (fd_flags >= 0) ::fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_storage bound = address;
    if (bound.ss_family == AF_INET)
        reinterpret_cast<sockaddr_in*>(&bound)->sin_port = htons(state.config.port);
    else
        reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port = htons(state.config.port);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&bound), address_length) != 0) {
        const std::error_code error = last_error();
        ::close(fd);
        throw std::system_error(error, "native listener bind");
    }
    if (::listen(fd, 511) != 0) {
        const std::error_code error = last_error();
        ::close(fd);
        throw std::system_error(error, "native listener listen");
    }
    state.fd = fd;
    // Port 0 is replaced by the actually bound port (interface contract).
    sockaddr_storage actual{};
    socklen_t actual_length = sizeof(actual);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&actual), &actual_length) == 0) {
        const uint16_t port = actual.ss_family == AF_INET6
            ? reinterpret_cast<sockaddr_in6*>(&actual)->sin6_port
            : reinterpret_cast<sockaddr_in*>(&actual)->sin_port;
        state.config.port = ntohs(port);
    }
}

// SSL-type listener: consume the accepted plain stream into a TLS connection.
// Handshake initiation stays with the session (async_handshake on the
// returned connection), matching the STARTTLS upgrade shape.
std::unique_ptr<IConnection> wrap_ssl_listener_connection(
    const std::shared_ptr<ListenerState>& state, std::unique_ptr<IConnection> connection) {
    auto* base = dynamic_cast<NativeStreamBase*>(connection.get());
    if (!base) throw std::runtime_error("native TLS listener requires a native stream");
    auto stream = base->release_state();
    connection.reset(); // the wrapper now owns nothing
    try {
        return state->hooks->wrap(state->loop, stream, state->tls->snapshot(),
                                  net::HandshakeRole::server);
    } catch (...) {
        stream->dispose(); // loop thread: close the socket we just consumed
        throw;
    }
}

// Loop thread: drain pending connections. Level-triggered readiness means the
// event repeats while connections wait, so a transient accept error must not
// leave the fd registered (it would spin).
void accept_ready(const std::shared_ptr<ListenerState>& state) {
    if (state->fd < 0) return;
    for (;;) {
        const int client = ::accept(state->fd, nullptr, nullptr);
        if (client >= 0) {
            std::unique_ptr<IConnection> connection;
            try {
                connection = make_native_tcp_connection(client, state->loop);
            } catch (const std::exception& error) {
                ::close(client); // nothing owns the fd yet
                LOG_NETWORK_ERROR("Accepted connection setup failed: {}", error.what());
                continue;
            }
            if (state->config.type == ListenerType::SSL) {
                try {
                    connection = wrap_ssl_listener_connection(state, std::move(connection));
                } catch (const std::exception& error) {
                    // The wrapper (or its stream) still owns the fd; its
                    // destructor schedules the teardown.
                    connection.reset();
                    LOG_NETWORK_ERROR("TLS connection setup failed: {}", error.what());
                    continue;
                }
            }
            // Deliver through the loop queue like every other callback. The
            // holder is copyable for std::function; exclusive ownership moves
            // back into the unique_ptr right before the handler runs.
            auto holder = std::make_shared<std::unique_ptr<IConnection>>(std::move(connection));
            state->loop->post([state, handler = state->handler, holder] {
                if (!handler) return;
                std::unique_ptr<IConnection> owned = std::move(*holder);
                handler(std::move(owned), state->config);
            });
            continue;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        LOG_NETWORK_ERROR("Accept failed: {}", last_error().message());
        if (state->registered) {
            state->loop->remove_fd(state->fd);
            state->registered = false;
        }
        return;
    }
}

void register_listener(const std::shared_ptr<ListenerState>& state) {
    if (state->registered || state->fd < 0) return;
    state->entry = std::make_shared<LoopEntry>();
    const std::weak_ptr<ListenerState> weak = state;
    state->entry->on_event = [weak](unsigned) {
        if (const auto self = weak.lock()) accept_ready(self);
    };
    if (state->loop->add_fd(state->fd, Ready::readable, state->entry)) state->registered = true;
}

void close_listener(const std::shared_ptr<ListenerState>& state) {
    if (state->fd >= 0) {
        if (state->registered) {
            state->loop->remove_fd(state->fd);
            state->registered = false;
        }
        ::close(state->fd);
        state->fd = -1;
    }
    state->handler = {};
}

class NativeServerTransport final : public net::IServerTransport {
public:
    NativeServerTransport(const net::ServerTransportConfig& config,
                          const std::vector<NativeLoopPtr>& loops)
        : tls_(std::make_shared<TlsContextHolder>()) {
        if (loops.empty())
            throw std::invalid_argument("native server transport requires a runtime loop");
        sockaddr_storage address{};
        const socklen_t address_length = parse_bind_address(config.address, address);
        if (address_length == 0)
            throw std::invalid_argument("native server transport requires a numeric bind address: " +
                                        config.address);
        hooks_ = tls_hook::get_hooks();
        const bool needs_tls = !config.tls.certificate_file.empty() || !config.tls.private_key_file.empty();
        bool has_ssl_listener = false;
        for (const auto& listener : config.listeners)
            has_ssl_listener |= listener.type == ListenerType::SSL;
        if ((needs_tls || has_ssl_listener) && !hooks_)
            throw std::system_error(std::make_error_code(std::errc::operation_not_supported),
                                    "native backend has no TLS support in this binary");
        if (hooks_ && (needs_tls || has_ssl_listener))
            tls_->store(hooks_->make_server_context(config.tls)); // throws on bad files
        for (const auto& listener : config.listeners) {
            if (listener.type != ListenerType::TCP && listener.type != ListenerType::SSL)
                throw std::system_error(std::make_error_code(std::errc::operation_not_supported),
                                        "native backend supports TCP and SSL listeners only");
            auto state = std::make_shared<ListenerState>();
            state->config = listener;
            state->loop = loops[listener_slot_++ % loops.size()];
            state->hooks = hooks_;
            state->tls = tls_;
            open_listen_socket(*state, address, address_length);
            bound_listeners_.push_back(state->config);
            listeners_.push_back(std::move(state));
        }
    }

    ~NativeServerTransport() override { stop(); }

    const std::vector<ListenerConfig>& listeners() const override { return bound_listeners_; }

    void start(AcceptHandler handler) override {
        std::lock_guard<std::mutex> lock(lifecycle_);
        if (stopped_) throw std::logic_error("listener has already stopped");
        if (started_) return;
        started_ = true;
        for (auto& state : listeners_) {
            const auto captured = state;
            // Handler and interest registration both run on the loop thread,
            // so a ready accept can never observe a missing handler.
            captured->loop->post([captured, handler] { captured->handler = handler; });
            captured->loop->post([captured] { register_listener(captured); });
        }
    }

    void stop() override {
        std::lock_guard<std::mutex> lock(lifecycle_);
        if (stopped_) return;
        stopped_ = true;
        for (auto& state : listeners_) {
            // Close and clear the handler on the loop thread. invoke_when_live
            // blocks until that task ran: accept callbacks queued before it
            // have been delivered (FIFO queue) and none can be queued after —
            // exactly the IServerTransport::stop contract.
            const auto captured = state;
            const auto close_job = [captured] { close_listener(captured); };
            if (!captured->loop->invoke_when_live(close_job)) close_job();
        }
    }

    void configure_tls(const net::TlsConfig& config) override {
        if (!hooks_)
            throw std::system_error(std::make_error_code(std::errc::operation_not_supported),
                                    "native backend has no TLS support in this binary");
        // Build fully before the swap: a failing configure leaves the previous
        // context serving.
        tls_->store(hooks_->make_server_context(config));
    }

    std::unique_ptr<IConnection> upgrade_tls(std::unique_ptr<IConnection> plain) override {
        const auto context = tls_->snapshot();
        if (!hooks_ || !context) {
            // Preserve the documented exception type; close what we were given.
            if (plain) plain->close();
            throw std::system_error(std::make_error_code(std::errc::operation_not_supported),
                                    !hooks_ ? "native backend has no TLS support in this binary"
                                            : "no TLS context configured (configure_tls first)");
        }
        auto* base = dynamic_cast<NativeStreamBase*>(plain.get());
        if (!base) {
            if (plain) plain->close();
            throw std::invalid_argument("upgrade_tls requires a native transport connection");
        }
        auto stream = base->release_state();
        plain.reset(); // the TLS connection owns the stream now
        return hooks_->wrap(stream->loop, std::move(stream), context, net::HandshakeRole::server);
    }

private:
    size_t listener_slot_ = 0;
    std::vector<std::shared_ptr<ListenerState>> listeners_;
    std::vector<ListenerConfig> bound_listeners_;
    tls_hook::Hooks* hooks_ = nullptr;
    std::shared_ptr<TlsContextHolder> tls_;
    std::mutex lifecycle_;
    bool started_ = false;
    bool stopped_ = false;
};
} // namespace

std::unique_ptr<net::IServerTransport> make_native_server_transport(
    const net::ServerTransportConfig& config, const std::vector<NativeLoopPtr>& loops) {
    return std::make_unique<NativeServerTransport>(config, loops);
}

} // namespace pr::net::native
