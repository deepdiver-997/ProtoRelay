// Native TCP transport contract tests against a real 127.0.0.1 listener:
// accept/connect, echo end-to-end, partial reads, write backpressure, full-
// buffer writes, EOF, cancel/watchdog exactly-once, close during connect, the
// listener stop contract, TLS refusal, and shutdown lifecycle. Every scenario
// runs on every compiled poller backend.
#undef NDEBUG
#include "framework/net/native_transport.h"

#include <cassert>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace pr;
using namespace pr::net;
using namespace pr::net::native;

namespace {

template <typename T>
bool ready_within(std::future<T>& future, std::chrono::milliseconds budget) {
    return future.wait_for(budget) == std::future_status::ready;
}

uint16_t base_port() {
    if (const char* configured = std::getenv("PROTORELAY_TEST_PORT_BASE")) {
        const int base = std::atoi(configured);
        if (base > 0) return static_cast<uint16_t>(base + 90);
    }
    return 20290;
}

int open_connected_socket(uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    return fd;
}

// ---- blocking test-side peer (plain BSD sockets) ----

struct RawPeer {
    int fd = -1;
    explicit RawPeer(int socket_fd) : fd(socket_fd) {}
    ~RawPeer() { close(); }
    RawPeer(const RawPeer&) = delete;
    RawPeer& operator=(const RawPeer&) = delete;

    void close() {
        if (fd >= 0) ::close(fd);
        fd = -1;
    }
    void send_all(const std::string& data) const {
        size_t sent = 0;
        while (sent < data.size()) {
            const ssize_t wrote = ::send(fd, data.data() + sent, data.size() - sent, 0);
            assert(wrote > 0 || (wrote < 0 && errno == EINTR));
            if (wrote > 0) sent += static_cast<size_t>(wrote);
        }
    }
    std::string recv_some() const {
        char buffer[4096];
        const ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
        if (got <= 0) return {};
        return std::string(buffer, static_cast<size_t>(got));
    }
    void recv_all(std::string& into, size_t total) const {
        while (into.size() < total) {
            const std::string chunk = recv_some();
            assert(!chunk.empty()); // EOF before the expected volume
            into += chunk;
        }
    }
};

// Listens on 127.0.0.1; SO_RCVBUF set before any connect (inherited by
// accepted sockets) so backpressure tests can fill the peer quickly.
struct RawListener {
    int fd = -1;
    uint16_t port = 0;
    explicit RawListener(uint16_t hint, int receive_buffer = 0) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        assert(fd >= 0);
        const int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (receive_buffer > 0)
            ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bool bound = false;
        for (int attempt = 0; attempt < 3 && !bound; ++attempt) {
            address.sin_port = htons(attempt == 0 ? hint : 0);
            bound = ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
            assert(bound || errno == EADDRINUSE);
        }
        assert(bound);
        assert(::listen(fd, 16) == 0);
        sockaddr_in actual{};
        socklen_t length = sizeof(actual);
        assert(::getsockname(fd, reinterpret_cast<sockaddr*>(&actual), &length) == 0);
        port = ntohs(actual.sin_port);
    }
    ~RawListener() { if (fd >= 0) ::close(fd); }
    RawListener(const RawListener&) = delete;
    RawListener& operator=(const RawListener&) = delete;
};

// Creates the native listener on the runtime, falling back to an ephemeral
// port when the preferred slot is taken by anything else on the machine.
std::unique_ptr<IServerTransport> make_listener(NativeRuntime& runtime, uint16_t hint,
                                                uint16_t& bound) {
    ServerTransportConfig config;
    config.address = "127.0.0.1";
    for (int attempt = 0; attempt < 3; ++attempt) {
        config.listeners.clear();
        config.listeners.emplace_back(ListenerType::TCP, attempt == 0 ? hint : static_cast<uint16_t>(0));
        try {
            auto transport = runtime.make_server_transport(config);
            bound = transport->listeners().front().port;
            return transport;
        } catch (const std::system_error& error) {
            assert(attempt < 2);
            assert(error.code() == std::errc::address_in_use);
        }
    }
    return nullptr; // unreachable
}

// Handoff of the single accepted connection a scenario works with.
struct AcceptSlot {
    std::promise<std::unique_ptr<IConnection>> promise;
    std::future<std::unique_ptr<IConnection>> future = promise.get_future();
};

// ---- echo session driven over the IConnection contract ----

struct EchoSession {
    std::unique_ptr<IConnection> connection;
    std::vector<char> buffer_storage = std::vector<char>(4096);
};

void pump_echo(const std::shared_ptr<EchoSession>& session) {
    session->connection->async_read(
        buffer(session->buffer_storage), [session](const std::error_code& error, std::size_t bytes) {
            if (error) return; // EOF or failure ends the echo
            session->connection->async_write(
                ConstBuffer(session->buffer_storage.data(), bytes),
                [session](const std::error_code& write_error, std::size_t) {
                    if (!write_error) pump_echo(session);
                });
        });
}

// ---- scenario: connect + echo both from the native client and a raw peer ----

void echo_roundtrip(PollerBackend backend, uint16_t hint) {
    NativeRuntime runtime(1, backend);
    runtime.start();
    uint16_t bound = 0;
    auto transport = make_listener(runtime, hint, bound);
    assert(bound != 0);

    std::atomic<int> accepted{0};
    transport->start([&accepted](std::unique_ptr<IConnection> connection, const ListenerConfig&) {
        accepted.fetch_add(1);
        auto session = std::make_shared<EchoSession>();
        session->connection = std::move(connection);
        pump_echo(session);
    });

    // The native client factory connects and exchanges data.
    auto client = runtime.make_client_transport();
    std::promise<std::error_code> connected;
    auto connected_outcome = connected.get_future();
    client->async_connect("127.0.0.1", bound, [&](const std::error_code& error) {
        connected.set_value(error);
    });
    assert(ready_within(connected_outcome, std::chrono::seconds(5)));
    assert(!connected_outcome.get());
    assert(client->is_open());
    assert(client->get_remote_ip() == "127.0.0.1");
    assert(client->get_local_port() != 0);

    std::promise<std::error_code> wrote;
    auto wrote_outcome = wrote.get_future();
    const std::string message = "PING-ECHO";
    client->async_write(buffer(message), [&](const std::error_code& error, std::size_t bytes) {
        assert(!error && bytes == message.size()); // whole buffer, once
        wrote.set_value(error);
    });
    assert(ready_within(wrote_outcome, std::chrono::seconds(5)));
    assert(!wrote_outcome.get());

    std::vector<char> received(message.size());
    std::promise<std::error_code> read_back;
    auto read_outcome = read_back.get_future();
    client->async_read(buffer(received), [&](const std::error_code& error, std::size_t bytes) {
        assert(bytes == message.size());
        read_back.set_value(error);
    });
    assert(ready_within(read_outcome, std::chrono::seconds(5)));
    assert(!read_outcome.get());
    assert(std::memcmp(received.data(), message.data(), message.size()) == 0);

    // A plain-socket peer proves the listener is a real TCP listener.
    {
        RawPeer peer(open_connected_socket(bound));
        peer.send_all("HELLO");
        std::string echoed;
        peer.recv_all(echoed, 5);
        assert(echoed == "HELLO");
        peer.close(); // server sees EOF and retires the session
    }

    // read after close completes with an error, not silence.
    std::promise<std::error_code> after_close;
    auto after_close_outcome = after_close.get_future();
    client->close();
    client->async_read(buffer(received), [&](const std::error_code& error, std::size_t) {
        after_close.set_value(error);
    });
    assert(ready_within(after_close_outcome, std::chrono::seconds(5)));
    assert(after_close_outcome.get() == std::make_error_code(std::errc::bad_file_descriptor));

    // stop() waits out queued accept callbacks: the raw peer's connection is
    // already counted when this returns, and none can arrive afterwards.
    transport->stop();
    assert(accepted.load() == 2);
    runtime.stop(true);
}

// ---- scenario: partial reads return exactly the available bytes ----

void partial_reads(PollerBackend backend, uint16_t hint) {
    NativeRuntime runtime(1, backend);
    runtime.start();
    uint16_t bound = 0;
    auto transport = make_listener(runtime, hint, bound);

    AcceptSlot slot;
    transport->start([&slot](std::unique_ptr<IConnection> connection, const ListenerConfig&) {
        slot.promise.set_value(std::move(connection));
    });

    RawPeer peer(open_connected_socket(bound));
    auto server_side = slot.future.get();

    auto read_bytes = [&](std::vector<char>& into) {
        std::promise<std::size_t> done;
        auto outcome = done.get_future();
        server_side->async_read(buffer(into), [&](const std::error_code& error, std::size_t bytes) {
            assert(!error);
            done.set_value(bytes);
        });
        assert(ready_within(outcome, std::chrono::seconds(5)));
        return outcome.get();
    };

    // One small write, one read: the read returns WHAT IS AVAILABLE, exactly.
    peer.send_all("abcd");
    std::vector<char> chunk(1024);
    assert(read_bytes(chunk) == 4);
    assert(std::memcmp(chunk.data(), "abcd", 4) == 0);

    peer.send_all("0123456789");
    assert(read_bytes(chunk) == 10);

    // Peer close -> the next read reports end_of_stream.
    peer.close();
    {
        std::promise<std::error_code> done;
        auto outcome = done.get_future();
        server_side->async_read(buffer(chunk), [&](const std::error_code& error, std::size_t) {
            done.set_value(error);
        });
        assert(ready_within(outcome, std::chrono::seconds(5)));
        assert(outcome.get() == make_error_code(IoErrc::end_of_stream));
    }
    server_side->close();
    transport->stop();
    runtime.stop(true);
}

// ---- scenario: write backpressure — completion only after the peer drains ----

void write_backpressure(PollerBackend backend, uint16_t hint) {
    NativeRuntime runtime(1, backend);
    runtime.start();

    // Peer never reads until we say so; a small RCVBUF fills the pipe fast.
    RawListener listener(static_cast<uint16_t>(hint + 1), 4096);
    auto client = runtime.make_client_transport();
    std::promise<std::error_code> connected;
    auto connected_outcome = connected.get_future();
    client->async_connect("127.0.0.1", listener.port, [&](const std::error_code& error) {
        connected.set_value(error);
    });
    assert(ready_within(connected_outcome, std::chrono::seconds(5)));
    assert(!connected_outcome.get());

    // Shrink our send buffer too.
    const int send_buffer = 4096;
    assert(client->native_handle() >= 0);
    ::setsockopt(client->native_handle(), SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer));

    const std::string payload(1 << 20, 'A'); // 1 MiB >> kernel buffers
    std::promise<std::error_code> wrote;
    auto wrote_outcome = wrote.get_future();
    std::atomic<std::size_t> written_bytes{0};
    std::atomic<int> write_completions{0};
    client->async_write(buffer(payload), [&](const std::error_code& error, std::size_t bytes) {
        write_completions.fetch_add(1);
        written_bytes.store(bytes);
        wrote.set_value(error);
    });
    // Kernel buffers are far smaller than 1 MiB: an immediate completion would
    // mean the write ignored backpressure.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    assert(write_completions.load() == 0);

    // Draining the peer lets the write finish with the FULL size, once.
    const int accepted_fd = ::accept(listener.fd, nullptr, nullptr);
    assert(accepted_fd >= 0);
    {
        RawPeer peer(accepted_fd);
        std::string drained;
        peer.recv_all(drained, payload.size());
    }
    assert(ready_within(wrote_outcome, std::chrono::seconds(10)));
    assert(!wrote_outcome.get());
    assert(written_bytes.load() == payload.size());
    assert(write_completions.load() == 1);
    client->close();
    runtime.stop(true);
}

// ---- scenario: watchdog timer + cancel completes the read exactly once and
// the connection stays usable afterwards ----

void cancel_exactly_once(PollerBackend backend, uint16_t hint) {
    NativeRuntime runtime(1, backend);
    runtime.start();
    uint16_t bound = 0;
    auto transport = make_listener(runtime, hint, bound);

    AcceptSlot slot;
    transport->start([&slot](std::unique_ptr<IConnection> connection, const ListenerConfig&) {
        slot.promise.set_value(std::move(connection));
    });

    RawPeer peer(open_connected_socket(bound));
    auto server_side = slot.future.get();
    auto executor = server_side->executor();
    assert(std::dynamic_pointer_cast<NativeLoop>(executor));

    std::atomic<int> read_completions{0};
    std::promise<std::error_code> read_result;
    auto read_outcome = read_result.get_future();
    std::vector<char> chunk(1024);
    server_side->async_read(buffer(chunk), [&](const std::error_code& error, std::size_t) {
        read_completions.fetch_add(1);
        read_result.set_value(error);
    });

    // Watchdog pattern: a timer armed ON the connection executor fires
    // cancel() (timers mutate on their executor).
    auto watchdog = executor->make_timer();
    std::promise<void> watchdog_fired;
    auto watchdog_outcome = watchdog_fired.get_future();
    executor->post([&] {
        watchdog->expires_after(std::chrono::milliseconds(50));
        watchdog->async_wait([&](const std::error_code& error) {
            assert(!error);
            server_side->cancel(); // not close: the connection survives
            watchdog_fired.set_value();
        });
    });
    assert(ready_within(read_outcome, std::chrono::seconds(5)));
    assert(read_outcome.get() == std::make_error_code(std::errc::operation_canceled));
    assert(read_completions.load() == 1);
    assert(server_side->is_open()); // cancel() must not close
    assert(ready_within(watchdog_outcome, std::chrono::seconds(5)));

    // Exactly once: no further completions arrive.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(read_completions.load() == 1);

    // The connection still works: a new read receives what the peer sends now.
    peer.send_all("still-here");
    std::promise<std::error_code> next_read;
    auto next_outcome = next_read.get_future();
    server_side->async_read(buffer(chunk), [&](const std::error_code& error, std::size_t bytes) {
        assert(bytes == 10);
        next_read.set_value(error);
    });
    assert(ready_within(next_outcome, std::chrono::seconds(5)));
    assert(!next_outcome.get());

    server_side->close();
    transport->stop();
    runtime.stop(true);
}

// ---- scenario: closing during a pending connect completes the handler once ----

void connect_cancel(PollerBackend backend, uint16_t hint) {
    NativeRuntime runtime(1, backend);
    runtime.start();
    // "localhost" is non-numeric: the detached resolver thread runs, then a
    // real connect to the listener starts. The close() below races both
    // stages; whatever wins, the handler must complete EXACTLY once, and a
    // canceled connect is the outcome whenever the close won.
    RawListener listener(static_cast<uint16_t>(hint + 3));
    auto client = runtime.make_client_transport();
    std::atomic<int> completions{0};
    std::promise<std::error_code> outcome;
    auto result = outcome.get_future();
    client->async_connect("localhost", listener.port, [&](const std::error_code& error) {
        completions.fetch_add(1);
        outcome.set_value(error);
    });
    client->close();
    assert(ready_within(result, std::chrono::seconds(5)));
    const std::error_code error = result.get();
    assert(completions.load() == 1);
    assert(error == std::make_error_code(std::errc::operation_canceled) || !error ||
           error == std::make_error_code(std::errc::connection_refused));
    assert(!client->is_open());
    runtime.stop(true);
}

// ---- scenario: listener contract (ports, TLS refusal, stop semantics) ----

void listener_contract(PollerBackend backend, uint16_t hint) {
    NativeRuntime runtime(1, backend);
    runtime.start();

    // TLS capabilities are refused loudly.
    {
        ServerTransportConfig config;
        config.address = "127.0.0.1";
        config.listeners.emplace_back(ListenerType::SSL, 0);
        bool threw = false;
        try {
            runtime.make_server_transport(config);
        } catch (const std::system_error& error) {
            threw = error.code() == std::errc::operation_not_supported;
        }
        assert(threw);
    }

    uint16_t bound = 0;
    auto transport = make_listener(runtime, hint, bound);
    assert(transport->listeners().front().port == bound); // port 0 replaced
    bool tls_threw = false;
    try {
        transport->configure_tls(TlsConfig{});
    } catch (const std::system_error& error) {
        tls_threw = error.code() == std::errc::operation_not_supported;
    }
    assert(tls_threw);

    std::atomic<int> accepted{0};
    transport->start([&](std::unique_ptr<IConnection>, const ListenerConfig&) {
        accepted.fetch_add(1);
    });
    transport->start([&](std::unique_ptr<IConnection>, const ListenerConfig&) {}); // no-op

    // stop() returns after queued accept callbacks ran; none can run after.
    RawPeer peer(open_connected_socket(bound));
    transport->stop();
    const int accepted_before = accepted.load();
    assert(accepted_before >= 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(accepted.load() == accepted_before);
    peer.close();
    runtime.stop(true);
}

// ---- scenario: runtime shutdown completes pending connection IO once ----

void shutdown_lifecycle(PollerBackend backend, uint16_t hint) {
    auto runtime = std::make_shared<NativeRuntime>(1, backend);
    runtime->start();
    uint16_t bound = 0;
    auto transport = make_listener(*runtime, hint, bound);

    std::atomic<int> completions{0};
    std::promise<std::error_code> pending_read;
    auto read_outcome = pending_read.get_future();
    // The accepted connection (with its pending read) is owned by a holder the
    // test keeps alive; runtime shutdown must still settle the operation.
    auto kept = std::make_shared<std::vector<std::unique_ptr<IConnection>>>();
    transport->start([&](std::unique_ptr<IConnection> connection, const ListenerConfig&) {
        std::vector<char> chunk(512);
        connection->async_read(buffer(chunk),
            [&completions, &pending_read](const std::error_code& error, std::size_t) {
                completions.fetch_add(1);
                pending_read.set_value(error);
            });
        kept->push_back(std::move(connection));
    });

    RawPeer peer(open_connected_socket(bound));
    // Give the loop time to accept and start the read (the peer stays silent,
    // so the read remains pending).
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    assert(read_outcome.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready);

    transport->stop();
    runtime->stop(true);
    // stop(true) delivered the canceled completion before returning.
    assert(ready_within(read_outcome, std::chrono::milliseconds(0)));
    assert(read_outcome.get() == std::make_error_code(std::errc::operation_canceled));
    assert(completions.load() == 1);
    peer.close();
}

void executor_affinity(PollerBackend backend, uint16_t hint) {
    NativeRuntime runtime(1, backend);
    runtime.start();
    uint16_t bound = 0;
    auto transport = make_listener(runtime, hint, bound);

    AcceptSlot slot;
    transport->start([&slot](std::unique_ptr<IConnection> connection, const ListenerConfig&) {
        slot.promise.set_value(std::move(connection));
    });

    RawPeer peer(open_connected_socket(bound));
    auto server_side = slot.future.get();
    auto executor = server_side->executor();

    peer.send_all("x");
    std::vector<char> chunk(64);
    std::thread::id completion_thread{};
    std::promise<void> done;
    auto done_future = done.get_future();
    server_side->async_read(buffer(chunk), [&](const std::error_code& error, std::size_t) {
        assert(!error);
        completion_thread = std::this_thread::get_id();
        done.set_value();
    });
    assert(ready_within(done_future, std::chrono::seconds(5)));
    std::thread::id executor_thread{};
    auto probe = std::make_shared<std::promise<void>>();
    auto probe_future = probe->get_future();
    executor->post([&] {
        executor_thread = std::this_thread::get_id();
        probe->set_value();
    });
    assert(ready_within(probe_future, std::chrono::seconds(5)));
    // The IO completion ran on the connection's own executor thread.
    assert(completion_thread == executor_thread);
    server_side->close();
    transport->stop();
    runtime.stop(true);
}

} // namespace

int main() {
    const auto backends = available_backends();
    assert(!backends.empty());
    const uint16_t hint = base_port();
    for (const PollerBackend backend : backends) {
        echo_roundtrip(backend, hint);
        partial_reads(backend, hint);
        write_backpressure(backend, hint);
        cancel_exactly_once(backend, hint);
        connect_cancel(backend, hint);
        listener_contract(backend, hint);
        shutdown_lifecycle(backend, hint);
        executor_affinity(backend, hint);
    }
    return 0;
}
