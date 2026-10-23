// Native TCP stream state machine: one nonblocking socket driven by one
// NativeLoop. See native_stream_internal.h for the threading contract.
#include "native/native_stream_internal.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace pr::net::native {
namespace {
std::error_code last_error() { return std::error_code(errno, std::generic_category()); }

void set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void set_close_on_exec(int fd) {
    const int flags = ::fcntl(fd, F_GETFD, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

// SIGPIPE would kill the process on a write to a peer-closed socket; suppress
// it per socket (macOS/BSD option, plus MSG_NOSIGNAL at the send site).
void suppress_sigpipe(int fd) {
#ifdef SO_NOSIGPIPE
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
    (void)fd;
#endif
}

std::error_code socket_option_error(int fd, int option) {
    int value = 0;
    socklen_t length = sizeof(value);
    if (::getsockopt(fd, SOL_SOCKET, option, &value, &length) != 0) return last_error();
    return value ? std::error_code(value, std::generic_category()) : std::error_code{};
}
} // namespace

StreamState::StreamState(NativeLoopPtr owner, int socket_fd)
    : loop(std::move(owner)), fd(socket_fd) {
    if (fd >= 0) {
        set_nonblocking(fd);
        set_close_on_exec(fd);
        suppress_sigpipe(fd);
        open.store(true, std::memory_order_release);
        cache_endpoints();
    }
}

void StreamState::initiate_read(net::MutableBuffer buffer, IConnection::ReadHandler handler) {
    loop->assert_loop_thread("async_read");
    if (fd < 0 || !open.load(std::memory_order_acquire)) {
        loop->post([handler = std::move(handler)] {
            handler(std::make_error_code(std::errc::bad_file_descriptor), 0);
        });
        return;
    }
    if (read_pending) throw std::logic_error("native connection already has a pending read");
    read_pending = true;
    read_buffer = buffer;
    read_handler = std::move(handler);
    update_interest();
}

void StreamState::initiate_write(const void* data, std::size_t size, IConnection::WriteHandler handler) {
    loop->assert_loop_thread("async_write");
    if (fd < 0 || !open.load(std::memory_order_acquire)) {
        loop->post([handler = std::move(handler)] {
            handler(std::make_error_code(std::errc::bad_file_descriptor), 0);
        });
        return;
    }
    if (write_pending) throw std::logic_error("native connection already has a pending write");
    write_pending = true;
    write_offset = 0;
    // Copy: a completion must never depend on caller storage outliving the
    // initiation, and backpressure holds the tail for an unbounded time.
    write_data.assign(static_cast<const char*>(data), size);
    write_handler = std::move(handler);
    update_interest();
}

void StreamState::begin_socket_connect(const sockaddr_storage& address, socklen_t length) {
    loop->assert_loop_thread("connect");
    // `connecting` was claimed by the client transport before resolution;
    // a cancel() that raced ahead would have closed the pending handler.
    if (!connecting) return;
    fd = ::socket(address.ss_family, SOCK_STREAM, 0);
    if (fd < 0) {
        connecting = false;
        complete_connect(last_error());
        return;
    }
    set_nonblocking(fd);
    set_close_on_exec(fd);
    suppress_sigpipe(fd);
#ifdef TCP_NODELAY
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#endif
    const int started = ::connect(fd, reinterpret_cast<const sockaddr*>(&address), length);
    if (started == 0) {
        connecting = false;
        open.store(true, std::memory_order_release);
        cache_endpoints();
        complete_connect(std::error_code{});
        return;
    }
    if (errno == EINPROGRESS) {
        update_interest(); // readable|writable until the outcome arrives
        return;
    }
    const std::error_code failure = last_error();
    connecting = false;
    complete_connect(failure);
}

void StreamState::initiate_cancel() {
    loop->assert_loop_thread("cancel");
    cancel_pending(std::make_error_code(std::errc::operation_canceled));
}

void StreamState::initiate_close() {
    loop->assert_loop_thread("close");
    open.store(false, std::memory_order_release);
    cancel_pending(std::make_error_code(std::errc::operation_canceled));
    if (fd >= 0) {
        if (registered) {
            loop->remove_fd(fd);
            registered = false;
        }
        interest = 0;
        ::close(fd);
        fd = -1;
    }
}

void StreamState::handle_event(unsigned ready) {
    if (fd < 0) return; // stale event from before a close in the same batch
    if (connecting) {
        // The connect outcome shows up as writable (success or SO_ERROR) and
        // possibly readable on failure; the socket option tells them apart.
        if (ready & (Ready::readable | Ready::writable)) finish_connect();
        return;
    }
    if (read_pending && (ready & Ready::readable)) do_read();
    if (fd >= 0 && write_pending && (ready & Ready::writable)) do_flush();
}

void StreamState::handle_loop_stopping() {
    // Runtime shutdown: pending operations complete canceled exactly once; the
    // object itself stays owned by its session.
    cancel_pending(std::make_error_code(std::errc::operation_canceled));
}

void StreamState::dispose() {
    loop->assert_loop_thread("dispose");
    cancel_pending(std::make_error_code(std::errc::operation_canceled));
    if (fd >= 0) {
        if (registered) {
            loop->remove_fd(fd);
            registered = false;
        }
        interest = 0;
        ::close(fd);
        fd = -1;
    }
}

void StreamState::do_read() {
    if (read_buffer.size() == 0) { // a zero-byte read is a success with 0
        read_pending = false;
        complete_read(std::error_code{}, 0);
        return;
    }
    for (;;) {
        const ssize_t got = ::read(fd, read_buffer.data(), read_buffer.size());
        if (got > 0) { // available bytes only — the partial-read contract
            read_pending = false;
            complete_read(std::error_code{}, static_cast<std::size_t>(got));
            return;
        }
        if (got == 0) {
            read_pending = false;
            complete_read(net::make_error_code(net::IoErrc::end_of_stream), 0);
            return;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return; // level-triggered: stay armed
        read_pending = false;
        complete_read(last_error(), 0);
        return;
    }
}

void StreamState::do_flush() {
    while (write_offset < write_data.size()) {
#ifdef MSG_NOSIGNAL
        const ssize_t sent = ::send(fd, write_data.data() + write_offset,
                                    write_data.size() - write_offset, MSG_NOSIGNAL);
#else
        const ssize_t sent = ::send(fd, write_data.data() + write_offset,
                                    write_data.size() - write_offset, 0);
#endif
        if (sent > 0) {
            write_offset += static_cast<std::size_t>(sent);
            continue;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return; // backpressure: stay armed
        const std::size_t transferred = write_offset;
        write_pending = false;
        complete_write(last_error(), transferred);
        return;
    }
    // Successful writes deliver the ENTIRE buffer, exactly one completion.
    const std::size_t total = write_data.size();
    write_data.clear();
    write_data.shrink_to_fit();
    write_pending = false;
    complete_write(std::error_code{}, total);
}

void StreamState::finish_connect() {
    connecting = false;
    const std::error_code outcome = socket_option_error(fd, SO_ERROR);
    update_interest(); // drop the connect interest
    if (outcome) {
        ::close(fd);
        fd = -1;
        open.store(false, std::memory_order_release);
        complete_connect(outcome);
        return;
    }
    open.store(true, std::memory_order_release);
    cache_endpoints();
    complete_connect(std::error_code{});
}

void StreamState::complete_read(const std::error_code& error, std::size_t bytes) {
    IConnection::ReadHandler handler;
    handler.swap(read_handler);
    read_buffer = net::MutableBuffer{};
    update_interest();
    loop->post([handler = std::move(handler), error, bytes] { handler(error, bytes); });
}

void StreamState::complete_write(const std::error_code& error, std::size_t bytes) {
    IConnection::WriteHandler handler;
    handler.swap(write_handler);
    update_interest();
    loop->post([handler = std::move(handler), error, bytes] { handler(error, bytes); });
}

void StreamState::complete_connect(const std::error_code& error) {
    IConnection::HandshakeHandler handler;
    handler.swap(connect_handler);
    update_interest();
    loop->post([handler = std::move(handler), error] { handler(error); });
}

void StreamState::cancel_pending(const std::error_code& error) {
    if (read_pending) {
        read_pending = false;
        complete_read(error, 0);
    }
    if (write_pending) {
        const std::size_t transferred = write_offset;
        write_pending = false;
        complete_write(error, transferred);
    }
    if (connecting) {
        connecting = false;
        complete_connect(error);
    }
}

void StreamState::ensure_entry() {
    if (entry) return;
    // First registration happens after construction, so shared_from_this is
    // valid; the closures hold only a weak reference and drop stale events
    // for a state that is already being torn down.
    entry = std::make_shared<LoopEntry>();
    const std::weak_ptr<StreamState> weak = shared_from_this();
    entry->on_event = [weak](unsigned ready) {
        if (const auto self = weak.lock()) self->handle_event(ready);
    };
    entry->on_loop_stopping = [weak] {
        if (const auto self = weak.lock()) self->handle_loop_stopping();
    };
}

void StreamState::update_interest() {
    if (fd < 0) return;
    ensure_entry();
    const unsigned want =
        (read_pending || connecting ? Ready::readable : 0) |
        (write_pending || connecting ? Ready::writable : 0);
    if (want == interest) return;
    if (want != 0) {
        if (registered && loop->mod_fd(fd, want)) {
            interest = want;
            return;
        }
        if (registered) {
            loop->remove_fd(fd);
            registered = false;
        }
        if (loop->add_fd(fd, want, entry)) {
            registered = true;
            interest = want;
        }
        return;
    }
    // Zero interest must deregister: poll reports POLLHUP/POLLERR regardless
    // of the requested events, which would busy-spin an idle closed peer.
    if (registered) {
        loop->remove_fd(fd);
        registered = false;
    }
    interest = 0;
}

void StreamState::cache_endpoints() {
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0) {
        const uint16_t port = address.ss_family == AF_INET6
            ? reinterpret_cast<sockaddr_in6*>(&address)->sin6_port
            : reinterpret_cast<sockaddr_in*>(&address)->sin_port;
        local_port = ntohs(port);
    }
    length = sizeof(address);
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0) {
        char text[INET6_ADDRSTRLEN] = {0};
        const char* converted = address.ss_family == AF_INET
            ? ::inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(&address)->sin_addr, text, sizeof(text))
            : ::inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(&address)->sin6_addr, text, sizeof(text));
        if (converted) remote_ip = text;
        const uint16_t port = address.ss_family == AF_INET6
            ? reinterpret_cast<sockaddr_in6*>(&address)->sin6_port
            : reinterpret_cast<sockaddr_in*>(&address)->sin_port;
        remote_port = ntohs(port);
    }
}

// ---------------------------------------------------------- NativeStreamBase

NativeStreamBase::~NativeStreamBase() {
    if (!state_) return;
    auto state = std::move(state_);
    state->open.store(false, std::memory_order_release);
    // Destruction from a foreign thread must not race the loop; the loop (or,
    // if it is already gone, this call) performs the teardown.
    state->loop->defer_destroy([state] { state->dispose(); });
}

void NativeStreamBase::run_on_loop(std::function<void()> action) {
    if (state_->loop->is_loop_thread()) action();
    else state_->loop->post(std::move(action));
}

std::unique_ptr<IConnection> make_native_tcp_connection(int fd, const NativeLoopPtr& loop) {
    class AcceptedConnection final : public NativeStreamBase {
    public:
        explicit AcceptedConnection(std::shared_ptr<StreamState> state)
            : NativeStreamBase(std::move(state)) {}
    };
    return std::make_unique<AcceptedConnection>(std::make_shared<StreamState>(loop, fd));
}

} // namespace pr::net::native
