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
#include <condition_variable>
#include <fcntl.h>
#include <cstring>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_set>
#include <utility>

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

// An accepted fd is not registered with a poller until its owner creates the
// stream. stop() can therefore cancel queued handoffs without waiting for a
// busy target loop, and without closing an fd already claimed by that loop.
struct PendingSocket {
    explicit PendingSocket(int value) : fd(value) {}
    ~PendingSocket() { close(); }
    void close() noexcept {
        const int value = fd.exchange(-1, std::memory_order_acq_rel);
        if (value >= 0) ::close(value);
    }
    int take() noexcept { return fd.exchange(-1, std::memory_order_acq_rel); }
    std::atomic<int> fd;
};

struct DeliveryState;
struct DeliveryLease {
    DeliveryLease() = default;
    DeliveryLease(DeliveryLease&& other) noexcept
        : state(std::move(other.state)), handler(std::move(other.handler)),
          fd(std::exchange(other.fd, -1)) {}
    DeliveryLease(const DeliveryLease&) = delete;
    DeliveryLease& operator=(const DeliveryLease&) = delete;
    ~DeliveryLease();
    std::shared_ptr<DeliveryState> state;
    net::IServerTransport::AcceptHandler handler;
    int fd = -1;
};

struct DeliveryState : std::enable_shared_from_this<DeliveryState> {
    void start(net::IServerTransport::AcceptHandler callback) {
        std::lock_guard<std::mutex> lock(mutex);
        handler = std::move(callback);
    }
    bool enqueue(const std::shared_ptr<PendingSocket>& socket) {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopped || !handler) return false;
        pending.insert(socket);
        return true;
    }
    void cancel(const std::shared_ptr<PendingSocket>& socket) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            pending.erase(socket);
        }
        socket->close();
    }
    DeliveryLease claim(const std::shared_ptr<PendingSocket>& socket) {
        DeliveryLease lease;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopped || pending.find(socket) == pending.end()) return lease;
            // Copy before taking fd ownership: a throwing function copy leaves
            // the queued socket available to cancellation.
            lease.handler = handler;
            lease.fd = socket->take();
            pending.erase(socket);
            lease.state = shared_from_this();
            ++active;
        }
        return lease;
    }
    void close() {
        std::unordered_set<std::shared_ptr<PendingSocket>> canceled;
        net::IServerTransport::AcceptHandler released;
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopped = true;
            canceled.swap(pending);
            released = std::move(handler);
        }
        for (auto& socket : canceled) socket->close();
        // Destroy user captures outside the gate, including captures that stop
        // or release the application listener itself.
    }
    void wait_idle() {
        std::unique_lock<std::mutex> lock(mutex);
        idle.wait(lock, [this] { return active == 0; });
    }
    void finish() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex);
            --active;
        }
        idle.notify_all();
    }
    std::mutex mutex;
    std::condition_variable idle;
    std::unordered_set<std::shared_ptr<PendingSocket>> pending;
    net::IServerTransport::AcceptHandler handler;
    std::size_t active = 0;
    bool stopped = false;
};

DeliveryLease::~DeliveryLease() {
    if (fd >= 0) ::close(fd);
    if (state) state->finish();
}

struct ConnectionAssignment {
    ConnectionAssignment(const std::vector<NativeLoopPtr>& owners,
                         std::shared_ptr<std::atomic<size_t>> shared_cursor)
        : cursor(shared_cursor ? std::move(shared_cursor) : std::make_shared<std::atomic<size_t>>(0)) {
        for (const auto& loop : owners) loops.push_back(loop);
    }
    NativeLoopPtr next() const {
        const auto index = cursor->fetch_add(1, std::memory_order_relaxed) % loops.size();
        auto loop = loops[index].lock();
        if (!loop) throw std::system_error(std::make_error_code(std::errc::operation_canceled));
        return loop;
    }
    std::vector<std::weak_ptr<NativeLoop>> loops;
    std::shared_ptr<std::atomic<size_t>> cursor;
};

struct ListenerState {
    ListenerConfig config;
    int fd = -1;
    NativeLoopPtr loop;
    std::shared_ptr<LoopEntry> entry;
    bool registered = false;
    // Multiple external stop callers may close inline after the loop exits.
    std::mutex close_mutex;
    std::shared_ptr<DeliveryState> delivery;
    std::shared_ptr<ConnectionAssignment> assignment;
    // Set by stop() on the caller's thread before the close job is queued: a
    // drain loop already running on the listener thread stops pulling new
    // connections immediately instead of after the queue reaches the close.
    std::atomic<bool> stop_requested{false};
    bool accepting = false;                       // loop-thread only
    tls_hook::Hooks* hooks = nullptr;             // null without the TLS module
    std::shared_ptr<TlsContextHolder> tls;
    ~ListenerState() { if (fd >= 0) ::close(fd); }
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
    tls_hook::Hooks* hooks, const std::shared_ptr<TlsContextHolder>& tls,
    std::unique_ptr<IConnection> connection) {
    auto* base = dynamic_cast<NativeStreamBase*>(connection.get());
    if (!base) throw std::runtime_error("native TLS listener requires a native stream");
    auto stream = base->release_state();
    connection.reset(); // the wrapper now owns nothing
    try {
        return hooks->wrap(stream->loop, stream, tls->snapshot(), net::HandshakeRole::server);
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
        // stop() may be called while this drain loop is running on the loop
        // thread (its close job is queued behind us). Stop accepting as soon
        // as the request is visible; callbacks already enqueued are dropped
        // at delivery time below.
        if (state->stop_requested.load(std::memory_order_acquire) || !state->accepting) return;
        const int client = ::accept(state->fd, nullptr, nullptr);
        if (client >= 0) {
            std::shared_ptr<PendingSocket> pending;
            try {
                pending = std::make_shared<PendingSocket>(client);
            } catch (...) {
                ::close(client);
                throw;
            }
            auto delivery = state->delivery;
            try {
                auto target = state->assignment->next();
                if (!delivery->enqueue(pending)) continue;
                // Do not capture ListenerState or a strong target-loop pointer:
                // an unpumped task queue must not own the loop that contains it.
                target->post([delivery, pending, owner = std::weak_ptr<NativeLoop>(target),
                              config = state->config, hooks = state->hooks, tls = state->tls] {
                    try {
                        auto lease = delivery->claim(pending);
                        if (!lease.state) return;
                        auto loop = owner.lock();
                        if (!loop) return;
                        auto connection = make_native_tcp_connection(lease.fd, loop);
                        lease.fd = -1; // successful construction consumed the fd
                        if (config.type == ListenerType::SSL)
                            connection = wrap_ssl_listener_connection(hooks, tls, std::move(connection));
                        lease.handler(std::move(connection), config);
                    } catch (const std::exception& error) {
                        delivery->cancel(pending);
                        LOG_NETWORK_ERROR("Accepted connection delivery failed: {}", error.what());
                    } catch (...) {
                        delivery->cancel(pending);
                        LOG_NETWORK_ERROR("Accepted connection delivery threw a non-standard exception");
                    }
                });
            } catch (const std::exception& error) {
                delivery->cancel(pending);
                LOG_NETWORK_ERROR("Accepted connection handoff failed: {}", error.what());
            } catch (...) {
                delivery->cancel(pending);
                throw;
            }
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
    if (state->stop_requested.load(std::memory_order_acquire) || state->registered || state->fd < 0) return;
    state->accepting = true;
    state->entry = std::make_shared<LoopEntry>();
    const std::weak_ptr<ListenerState> weak = state;
    state->entry->on_event = [weak](unsigned) {
        if (const auto self = weak.lock()) accept_ready(self);
    };
    if (state->loop->add_fd(state->fd, Ready::readable, state->entry)) state->registered = true;
}

void close_listener(const std::shared_ptr<ListenerState>& state) {
    std::lock_guard<std::mutex> lock(state->close_mutex);
    state->accepting = false;
    if (state->fd >= 0) {
        if (state->registered) {
            state->loop->remove_fd(state->fd);
            state->registered = false;
        }
        ::close(state->fd);
        state->fd = -1;
    }
}

class NativeServerTransport final : public net::IServerTransport {
public:
    NativeServerTransport(const net::ServerTransportConfig& config,
                          const std::vector<NativeLoopPtr>& loops,
                          std::shared_ptr<std::atomic<size_t>> cursor)
        : connection_loops_(loops),
          assignment_(std::make_shared<ConnectionAssignment>(loops, std::move(cursor))),
          delivery_(std::make_shared<DeliveryState>()), tls_(std::make_shared<TlsContextHolder>()) {
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
            state->assignment = assignment_;
            state->delivery = delivery_;
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
        delivery_->start(std::move(handler));
        started_ = true;
        for (auto& state : listeners_) {
            const auto captured = state;
            captured->loop->post([captured] { register_listener(captured); });
        }
    }

    void stop() override {
        {
            std::lock_guard<std::mutex> lock(lifecycle_);
            if (shutdown_complete_) return;
            stopped_ = true;
        }
        for (const auto& state : listeners_)
            state->stop_requested.store(true, std::memory_order_release);
        delivery_->close(); // cancel queued raw fds before waiting on any loop

        const bool from_loop = NativeLoop::is_any_loop_thread();
        for (const auto& state : listeners_) {
            const auto captured = state;
            const auto close_job = [captured] { close_listener(captured); };
            if (from_loop) {
                // A callback may stop/release its transport. Never wait across
                // loops here: another loop can be in the same callback/stop path.
                captured->loop->defer_destroy(close_job);
            } else if (!captured->loop->invoke_when_live(close_job)) {
                close_job();
            }
        }
        if (from_loop) return; // the external stop contract is a blocking barrier
        delivery_->wait_idle();
        std::lock_guard<std::mutex> lock(lifecycle_);
        shutdown_complete_ = true;
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
        const auto loop = stream->loop;
        return hooks_->wrap(loop, std::move(stream), context, net::HandshakeRole::server);
    }

private:
    size_t listener_slot_ = 0;
    std::vector<NativeLoopPtr> connection_loops_;
    std::shared_ptr<ConnectionAssignment> assignment_;
    std::shared_ptr<DeliveryState> delivery_;
    std::vector<std::shared_ptr<ListenerState>> listeners_;
    std::vector<ListenerConfig> bound_listeners_;
    tls_hook::Hooks* hooks_ = nullptr;
    std::shared_ptr<TlsContextHolder> tls_;
    std::mutex lifecycle_;
    bool started_ = false;
    bool stopped_ = false;
    bool shutdown_complete_ = false;
};
} // namespace

std::unique_ptr<net::IServerTransport> make_native_server_transport(
    const net::ServerTransportConfig& config, const std::vector<NativeLoopPtr>& loops,
    std::shared_ptr<std::atomic<size_t>> connection_cursor) {
    return std::make_unique<NativeServerTransport>(config, loops, std::move(connection_cursor));
}

} // namespace pr::net::native
