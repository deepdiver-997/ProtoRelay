// Native TCP transport contract tests against a real 127.0.0.1 listener:
// accept/connect, echo end-to-end, partial reads, write backpressure, full-
// buffer writes, EOF, cancel/watchdog exactly-once, close during connect, the
// listener stop contract, TLS refusal, and shutdown lifecycle. Every scenario
// runs on every compiled poller backend.
#undef NDEBUG
#include "framework/net/native_transport.h"

#include <cassert>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
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

// Non-asserting connect used by the stop/accept race: after stop() the
// listener is gone, so refusal is the expected outcome.
int try_connect(uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return -1;
    }
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

// These probes do not consume next_executor(): observing a loop must not alter
// the runtime-wide connection assignment sequence being tested.
size_t loop_index(NativeRuntime& runtime, const std::shared_ptr<IExecutor>& executor) {
    for (size_t index = 0; index < runtime.thread_count(); ++index) {
        if (executor.get() == runtime.loop_for(index).get()) return index;
    }
    assert(false && "connection executor does not belong to its runtime");
    return 0;
}

std::vector<std::thread::id> probe_loop_threads(NativeRuntime& runtime) {
    std::vector<std::thread::id> threads(runtime.thread_count());
    for (size_t index = 0; index < threads.size(); ++index) {
        auto loop = runtime.loop_for(index);
        std::promise<void> probed;
        auto outcome = probed.get_future();
        loop->post([&, index, loop] {
            assert(loop->is_loop_thread());
            threads[index] = std::this_thread::get_id();
            probed.set_value();
        });
        assert(ready_within(outcome, std::chrono::seconds(5)));
        assert(threads[index] != std::thread::id{});
        for (size_t previous = 0; previous < index; ++previous)
            assert(threads[index] != threads[previous]);
    }
    return threads;
}

// A bounded readiness wait keeps the new regression tests from hanging on a
// missing write or pending-fd close. EINTR does not restart the five-second
// budget. Callers use MSG_DONTWAIT even after readiness to avoid an unbounded
// test-side recv().
void wait_for_peer_event(int fd) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        const auto remaining = deadline - std::chrono::steady_clock::now();
        assert(remaining > std::chrono::steady_clock::duration::zero());
        const int timeout = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count()) + 1;
        pollfd descriptor{fd, POLLIN, 0};
        const int ready = ::poll(&descriptor, 1, timeout);
        if (ready < 0 && errno == EINTR) continue;
        assert(ready == 1);
        assert((descriptor.revents & POLLNVAL) == 0);
        assert((descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0);
        return;
    }
}

void assert_peer_closed(const RawPeer& peer) {
    wait_for_peer_event(peer.fd);
    char byte = 0;
    const ssize_t received = ::recv(peer.fd, &byte, 1, MSG_DONTWAIT);
    assert(received == 0 || (received < 0 && errno == ECONNRESET));
}

// Exercise both IO directions, not just the executor pointer or a relocated
// AcceptHandler. Initiations and completions must all use the accepted
// connection's actual owner loop. One-byte transfers avoid partial-read
// assumptions, and every wait has a five-second upper bound.
void assert_connection_io_affinity(IConnection& connection, const RawPeer& peer,
                                   const NativeLoopPtr& owner,
                                   std::thread::id expected_thread) {
    assert(connection.executor().get() == owner.get());
    assert(connection.is_open());
    std::vector<char> received(1);
    std::promise<void> read_done;
    auto read_outcome = read_done.get_future();
    owner->post([&] {
        assert(owner->is_loop_thread());
        assert(std::this_thread::get_id() == expected_thread);
        connection.async_read(buffer(received), [&](const std::error_code& error, size_t bytes) {
            assert(!error && bytes == 1);
            assert(owner->is_loop_thread());
            assert(std::this_thread::get_id() == expected_thread);
            read_done.set_value();
        });
    });
    peer.send_all("r");
    assert(ready_within(read_outcome, std::chrono::seconds(5)));
    assert(received.front() == 'r');

    const std::string reply = "w";
    std::promise<void> write_done;
    auto write_outcome = write_done.get_future();
    owner->post([&] {
        assert(owner->is_loop_thread());
        assert(std::this_thread::get_id() == expected_thread);
        connection.async_write(buffer(reply), [&](const std::error_code& error, size_t bytes) {
            assert(!error && bytes == 1);
            assert(owner->is_loop_thread());
            assert(std::this_thread::get_id() == expected_thread);
            write_done.set_value();
        });
    });
    assert(ready_within(write_outcome, std::chrono::seconds(5)));
    wait_for_peer_event(peer.fd);
    char written = 0;
    assert(::recv(peer.fd, &written, 1, MSG_DONTWAIT) == 1);
    assert(written == 'w');
}

void close_on_owner(IConnection& connection) {
    std::promise<void> closed;
    auto outcome = closed.get_future();
    connection.executor()->post([&] {
        connection.close();
        closed.set_value();
    });
    assert(ready_within(outcome, std::chrono::seconds(5)));
}

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
    const std::error_code connect_outcome = connected_outcome.get();
    if (connect_outcome)
        std::fprintf(stderr, "echo_roundtrip connect to 127.0.0.1:%u failed: %s (value=%d)\n",
                     bound, connect_outcome.message().c_str(), connect_outcome.value());
    assert(!connect_outcome);
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

    // connect() completing only queues the peer in the kernel backlog; the
    // accept callback is dispatched by the loop, so wait for it before stop.
    RawPeer peer(open_connected_socket(bound));
    const auto accept_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (accepted.load() < 1 && std::chrono::steady_clock::now() < accept_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // stop() returns after queued accept callbacks ran; none can run after.
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

// A single listener must spread accepted sockets over the runtime, not pin
// them to the listener's registration loop. A prior next_executor() call also
// makes the test verify the shared cursor without assuming an initial slot.
void accepted_connections_round_robin(PollerBackend backend, uint16_t hint) {
    NativeRuntime runtime(3, backend);
    runtime.start();
    const auto threads = probe_loop_threads(runtime);
    uint16_t bound = 0;
    auto transport = make_listener(runtime, hint, bound);
    const size_t anchor = loop_index(runtime, runtime.next_executor());

    std::array<AcceptSlot, 6> slots;
    std::atomic<size_t> accepted{0};
    transport->start([&](std::unique_ptr<IConnection> connection, const ListenerConfig& listener) {
        assert(listener.port == bound);
        const size_t owner = loop_index(runtime, connection->executor());
        assert(runtime.loop_for(owner)->is_loop_thread());
        assert(std::this_thread::get_id() == threads[owner]);
        const size_t index = accepted.fetch_add(1);
        assert(index < slots.size());
        slots[index].promise.set_value(std::move(connection));
    });

    std::array<size_t, 3> counts{};
    std::array<size_t, 6> owners{};
    for (size_t index = 0; index < slots.size(); ++index) {
        // Native clients would consume the same cursor and mask/change the
        // server distribution. There is exactly one outstanding raw accept.
        RawPeer peer(open_connected_socket(bound));
        assert(ready_within(slots[index].future, std::chrono::seconds(5)));
        auto connection = slots[index].future.get();
        assert(connection);
        owners[index] = loop_index(runtime, connection->executor());
        assert(owners[index] == (anchor + 1 + index) % counts.size());
        ++counts[owners[index]];
        if (index >= counts.size()) assert(owners[index] == owners[index - counts.size()]);
        assert_connection_io_affinity(*connection, peer, runtime.loop_for(owners[index]),
                                      threads[owners[index]]);
        close_on_owner(*connection);
    }
    for (const size_t count : counts) assert(count == 2);
    assert(accepted.load() == slots.size());
    transport->stop();
    runtime.stop(true);
    assert(runtime.exceptions_total() == 0);
}

// Interleave two independently created transports and direct executor requests.
// Per-listener/per-transport cursors, or a separate accept-only runtime cursor,
// cannot satisfy this one global sequence. Connections are still raw peers.
void server_transports_share_runtime_cursor(PollerBackend backend, uint16_t hint) {
    NativeRuntime runtime(3, backend);
    runtime.start();
    const auto threads = probe_loop_threads(runtime);
    std::array<uint16_t, 2> bound{};
    std::array<std::unique_ptr<IServerTransport>, 2> transports;
    for (size_t index = 0; index < transports.size(); ++index)
        transports[index] = make_listener(runtime, static_cast<uint16_t>(hint + index), bound[index]);
    size_t expected = (loop_index(runtime, runtime.next_executor()) + 1) % threads.size();

    std::array<AcceptSlot, 6> slots;
    std::atomic<size_t> accepted{0};
    for (size_t server = 0; server < transports.size(); ++server) {
        transports[server]->start([&, server](std::unique_ptr<IConnection> connection,
                                            const ListenerConfig& listener) {
            assert(listener.port == bound[server]);
            const size_t owner = loop_index(runtime, connection->executor());
            assert(runtime.loop_for(owner)->is_loop_thread());
            assert(std::this_thread::get_id() == threads[owner]);
            const size_t index = accepted.fetch_add(1);
            assert(index < slots.size());
            assert(index % transports.size() == server);
            slots[index].promise.set_value(std::move(connection));
        });
    }

    for (size_t index = 0; index < slots.size(); ++index) {
        RawPeer peer(open_connected_socket(bound[index % transports.size()]));
        assert(ready_within(slots[index].future, std::chrono::seconds(5)));
        auto connection = slots[index].future.get();
        assert(connection);
        const size_t owner = loop_index(runtime, connection->executor());
        assert(owner == expected);
        expected = (expected + 1) % threads.size();
        assert_connection_io_affinity(*connection, peer, runtime.loop_for(owner), threads[owner]);
        close_on_owner(*connection);

        assert(loop_index(runtime, runtime.next_executor()) == expected);
        expected = (expected + 1) % threads.size();
    }
    assert(accepted.load() == slots.size());
    for (auto& transport : transports) transport->stop();
    runtime.stop(true);
    assert(runtime.exceptions_total() == 0);
}

// A host name resolving to several addresses must try them all. "localhost"
// usually yields ::1 before 127.0.0.1; the listener below is IPv4-only, so a
// first-endpoint-only implementation fails exactly here (Asio tries the whole
// resolver result).
void hostname_falls_back_to_next_address(PollerBackend backend, uint16_t hint) {
    NativeRuntime runtime(1, backend);
    runtime.start();
    uint16_t bound = 0;
    auto transport = make_listener(runtime, hint, bound);

    AcceptSlot slot;
    transport->start([&slot](std::unique_ptr<IConnection> connection, const ListenerConfig&) {
        slot.promise.set_value(std::move(connection));
    });

    auto client = runtime.make_client_transport();
    std::promise<std::error_code> connected;
    auto connected_outcome = connected.get_future();
    client->async_connect("localhost", bound,
                          [&](const std::error_code& error) { connected.set_value(error); });
    assert(ready_within(connected_outcome, std::chrono::seconds(5)));
    assert(!connected_outcome.get());
    assert(client->get_remote_ip() == "127.0.0.1");

    auto server_side = slot.future.get();
    assert(server_side);
    client->close();
    server_side->close();
    transport->stop();
    runtime.stop(true);
}

// Keep the listener manually driven, with an active callback on loop 1 and
// an accepted-but-undelivered socket queued behind a blocker on loop 2. This
// distinguishes callback leases (which stop must await) from pending handoffs
// (which stop must cancel without waiting for their target loop to drain).
void stop_waits_for_active_accept_not_queued_target(PollerBackend backend, uint16_t hint) {
    NativeRuntime runtime(3, backend); // deliberately do not runtime.start()
    const auto listener_loop = runtime.loop_for(0);
    const auto active_loop = runtime.loop_for(1);
    const auto queued_loop = runtime.loop_for(2);
    uint16_t bound = 0;
    auto transport = make_listener(runtime, hint, bound);

    AcceptSlot active;
    std::atomic<int> accepted{0};
    std::atomic<bool> stopped{false};
    std::atomic<int> delivered_after_stop{0};
    std::promise<void> release_active;
    auto active_release = release_active.get_future();
    std::promise<void> active_finished;
    auto active_done = active_finished.get_future();
    std::thread::id active_thread{};
    transport->start([&](std::unique_ptr<IConnection> connection, const ListenerConfig&) {
        if (stopped.load(std::memory_order_acquire)) delivered_after_stop.fetch_add(1);
        assert(accepted.fetch_add(1) == 0); // the second handoff must never deliver
        assert(connection->executor().get() == active_loop.get());
        assert(active_loop->is_loop_thread());
        active_thread = std::this_thread::get_id();
        active.promise.set_value(std::move(connection));
        assert(ready_within(active_release, std::chrono::seconds(5)));
        active_finished.set_value();
    });
    // A single TCP listener registers on loop 0. Complete its start jobs before
    // any peer connects; only this test thread ever drives the listener loop.
    listener_loop->poll_once(0);

    // Align relative to the observed cursor, rather than assuming that a fresh
    // runtime/factory starts at zero. The next two accepted sockets select the
    // two non-listener loops, so neither test blocker can deadlock acceptance.
    bool aligned = false;
    for (size_t attempt = 0; attempt < runtime.thread_count(); ++attempt) {
        if (runtime.next_executor().get() == listener_loop.get()) {
            aligned = true;
            break;
        }
    }
    assert(aligned);

    std::promise<void> queued_entered;
    auto queued_blocked = queued_entered.get_future();
    std::promise<void> release_queued;
    auto queued_release = release_queued.get_future();
    queued_loop->post([&] {
        assert(queued_loop->is_loop_thread());
        queued_entered.set_value();
        assert(ready_within(queued_release, std::chrono::seconds(5)));
    });
    std::promise<void> active_loop_exited, queued_loop_exited;
    auto active_exit = active_loop_exited.get_future();
    auto queued_exit = queued_loop_exited.get_future();
    std::thread active_worker([&] {
        active_loop->run();
        active_loop_exited.set_value();
    });
    std::thread queued_worker([&] {
        queued_loop->run();
        queued_loop_exited.set_value();
    });
    assert(ready_within(queued_blocked, std::chrono::seconds(5)));

    RawPeer delivered_peer(open_connected_socket(bound));
    listener_loop->poll_once(5000);
    assert(ready_within(active.future, std::chrono::seconds(5)));
    auto kept_connection = active.future.get();
    assert(kept_connection);

    RawPeer pending_peer(open_connected_socket(bound));
    listener_loop->poll_once(5000);
    // connect() alone proves only kernel backlog membership. The listener poll
    // has returned, and the shared cursor advanced past loop 2: accept_ready
    // actually accepted this second fd and posted its handoff. Loop 2 is still
    // inside the earlier blocker, so the handoff cannot have started.
    assert(runtime.next_executor().get() == listener_loop.get());
    assert(accepted.load() == 1);
    assert(!ready_within(active_done, std::chrono::milliseconds(0)));

    std::promise<void> stop_finished;
    auto stop_done = stop_finished.get_future();
    std::thread stopper([&] {
        transport->stop();
        stopped.store(true, std::memory_order_release);
        stop_finished.set_value();
    });
    // EOF/reset while the target is blocked proves stop entered its Gate close
    // and closed the raw pending fd, not merely that the stopper was scheduled.
    // A bounded negative wait checks that an active callback still prevents
    // return; it does not orchestrate the race (the promises and EOF do that).
    assert_peer_closed(pending_peer);
    assert(!ready_within(stop_done, std::chrono::milliseconds(100)));
    assert(!ready_within(active_done, std::chrono::milliseconds(0)));
    release_active.set_value();
    assert(ready_within(stop_done, std::chrono::seconds(5)));
    assert(ready_within(active_done, std::chrono::milliseconds(0)));
    stopper.join();
    assert(stopped.load(std::memory_order_acquire));
    assert(accepted.load() == 1);
    // stop returned before loop 2 was unblocked: it must not wait for a pending
    // target task, nor defer that task's fd close until the task finally runs.
    assert(!ready_within(queued_release, std::chrono::milliseconds(0)));

    std::promise<void> fenced;
    auto fence_done = fenced.get_future();
    queued_loop->post([&] { fenced.set_value(); });
    release_queued.set_value();
    assert(ready_within(fence_done, std::chrono::seconds(5)));
    // The handoff was enqueued before this FIFO fence. It has now been drained
    // after stop returned, yet it must not invoke the application callback.
    assert(accepted.load() == 1);
    assert(delivered_after_stop.load() == 0);

    // An already delivered connection is not owned by the transport's pending
    // gate: stopping the listener leaves it open and usable on its owner loop.
    assert_connection_io_affinity(*kept_connection, delivered_peer, active_loop, active_thread);
    close_on_owner(*kept_connection);
    kept_connection.reset();
    transport->stop(); // idempotent after pending/active gate shutdown
    runtime.stop(true);
    assert(ready_within(active_exit, std::chrono::seconds(5)));
    assert(ready_within(queued_exit, std::chrono::seconds(5)));
    active_worker.join();
    queued_worker.join();
    listener_loop->poll_once(0); // settle the manually driven loop's shutdown
    assert(runtime.exceptions_total() == 0);
}

// Rebinding the exact port (not just accepting make_listener's fallback) proves
// the old listener has really closed, without a connect/fd hammer.
void assert_listener_port_released(uint16_t port) {
    RawListener replacement(port);
    assert(replacement.port == port);
}

// A stop called from ANY native loop must be nonblocking, including a loop in
// a different runtime. Run the same gates in an AcceptHandler or an actual
// grave disposer: holding the graves lock while invoking a disposer used to
// deadlock the two loops as each stop deferred work to the other loop.
void handlers_stop_other_runtime_without_deadlock(PollerBackend backend, uint16_t hint,
                                                 bool from_grave) {
    NativeRuntime first_runtime(1, backend);
    NativeRuntime second_runtime(1, backend);
    std::array<NativeRuntime*, 2> runtimes{&first_runtime, &second_runtime};
    std::array<uint16_t, 2> bound{};
    std::array<std::unique_ptr<IServerTransport>, 2> transports;
    for (size_t index = 0; index < runtimes.size(); ++index) {
        runtimes[index]->start();
        transports[index] = make_listener(*runtimes[index],
                                           static_cast<uint16_t>(hint + index), bound[index]);
    }

    std::array<std::promise<void>, 2> entered, stop_returned, context_finished;
    std::array<std::future<void>, 2> entries, returns, contexts;
    std::array<std::atomic<int>, 2> accepted{};
    std::array<std::atomic<bool>, 2> handler_returning{};
    std::promise<void> start_mutual_stop, release_contexts;
    const auto mutual_stop_gate = start_mutual_stop.get_future().share();
    const auto context_release = release_contexts.get_future().share();
    const auto gated_mutual_stop = [&](size_t index) {
        assert(runtimes[index]->loop_for(0)->is_loop_thread());
        assert(NativeLoop::is_any_loop_thread());
        assert(handler_returning[index].load(std::memory_order_acquire) == from_grave);
        // These signals belong to the tested context itself, not merely the
        // AcceptHandler that enqueues a disposer and immediately returns.
        entered[index].set_value();
        assert(mutual_stop_gate.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        transports[1 - index]->stop();
        stop_returned[index].set_value();
        // Both context bodies remain live until BOTH stop calls returned. In
        // the grave variant, both loops really are inside their disposers.
        assert(context_release.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        context_finished[index].set_value();
    };
    for (size_t index = 0; index < transports.size(); ++index) {
        entries[index] = entered[index].get_future();
        returns[index] = stop_returned[index].get_future();
        contexts[index] = context_finished[index].get_future();
        transports[index]->start([&, index](std::unique_ptr<IConnection> connection,
                                            const ListenerConfig&) {
            assert(accepted[index].fetch_add(1) == 0);
            const auto loop = runtimes[index]->loop_for(0);
            assert(connection->executor().get() == loop.get());
            assert(loop->is_loop_thread());
            if (from_grave) {
                // Keep the delivered connection alive for the identical gated
                // body in either variant; defer_destroy's callable is copyable.
                const auto kept = std::shared_ptr<IConnection>(std::move(connection));
                loop->defer_destroy([&, index, kept] {
                    assert(kept->is_open());
                    gated_mutual_stop(index);
                });
            } else {
                gated_mutual_stop(index);
            }
            // A grave must not run inline at defer_destroy's call site.
            handler_returning[index].store(true, std::memory_order_release);
        });
    }

    RawPeer first_peer(open_connected_socket(bound[0]));
    RawPeer second_peer(open_connected_socket(bound[1]));
    for (auto& entry : entries) assert(ready_within(entry, std::chrono::seconds(5)));
    start_mutual_stop.set_value();
    for (auto& returned : returns) assert(ready_within(returned, std::chrono::seconds(5)));
    assert(!NativeLoop::is_any_loop_thread());

    std::array<std::promise<void>, 2> external_started, external_finished;
    std::array<std::future<void>, 2> external_entries, external_returns;
    std::array<std::thread, 2> stoppers;
    for (size_t index = 0; index < transports.size(); ++index) {
        external_entries[index] = external_started[index].get_future();
        external_returns[index] = external_finished[index].get_future();
        stoppers[index] = std::thread([&, index] {
            assert(!NativeLoop::is_any_loop_thread());
            external_started[index].set_value();
            transports[index]->stop();
            external_finished[index].set_value();
        });
    }
    for (auto& entry : external_entries) assert(ready_within(entry, std::chrono::seconds(5)));
    for (size_t index = 0; index < transports.size(); ++index) {
        assert(!ready_within(external_returns[index], std::chrono::milliseconds(100)));
        assert(!ready_within(contexts[index], std::chrono::milliseconds(0)));
    }
    release_contexts.set_value();
    for (size_t index = 0; index < transports.size(); ++index) {
        assert(ready_within(external_returns[index], std::chrono::seconds(5)));
        assert(ready_within(contexts[index], std::chrono::milliseconds(0)));
        stoppers[index].join();
        assert(accepted[index].load() == 1);
        assert_listener_port_released(bound[index]);
        runtimes[index]->stop(true);
        assert(runtimes[index]->exceptions_total() == 0);
    }
}

// Pipes allocated before/after stop are low-volume fd canaries, not a claim to
// deterministically detect every data race. Nonblocking checks cannot hang if a
// bad repeated close invalidates/reuses one of their descriptors.
struct PipeCanary {
    std::array<int, 2> descriptors{-1, -1};
    PipeCanary() {
        assert(::pipe(descriptors.data()) == 0);
        for (const int fd : descriptors) {
            const int flags = ::fcntl(fd, F_GETFL, 0);
            assert(flags >= 0);
            assert(::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
        }
    }
    ~PipeCanary() {
        for (const int fd : descriptors) ::close(fd);
    }
    PipeCanary(const PipeCanary&) = delete;
    PipeCanary& operator=(const PipeCanary&) = delete;
    void assert_intact() const {
        for (const int fd : descriptors) {
            const int flags = ::fcntl(fd, F_GETFL, 0);
            assert(flags >= 0 && (flags & O_NONBLOCK) != 0);
        }
        const char sent = 'c';
        assert(::write(descriptors[1], &sent, 1) == 1);
        char received = 0;
        assert(::read(descriptors[0], &received, 1) == 1);
        assert(received == sent);
    }
};

// With no live loop, invoke_when_live falls back to close_listener on each
// caller's thread. Concurrent external stops must serialize that entire close
// even when other callers already returned and the fd number can be reused.
void concurrent_external_stop_without_live_loop(PollerBackend backend, uint16_t hint) {
    for (const bool previously_started : {false, true}) {
        NativeRuntime runtime(1, backend);
        if (previously_started) runtime.start();
        uint16_t bound = 0;
        auto transport = make_listener(runtime, hint, bound);
        std::atomic<int> accepted{0};
        transport->start([&](std::unique_ptr<IConnection>, const ListenerConfig&) {
            accepted.fetch_add(1);
        });
        if (previously_started) {
            // This FIFO probe proves start's registration job ran before the
            // runtime is stopped. Leave transport.stop to the racing callers.
            probe_loop_threads(runtime);
            runtime.stop(true);
        }
        assert(!runtime.is_running());
        assert(!NativeLoop::is_any_loop_thread());

        constexpr size_t kStoppers = 8;
        PipeCanary existing_fd;
        std::array<std::unique_ptr<PipeCanary>, kStoppers> reused_fds;
        std::array<std::promise<void>, kStoppers> entered, finished;
        std::array<std::future<void>, kStoppers> entries, completions;
        std::array<std::thread, kStoppers> stoppers;
        std::promise<void> release_stoppers;
        const auto start_gate = release_stoppers.get_future().share();
        for (size_t index = 0; index < kStoppers; ++index) {
            entries[index] = entered[index].get_future();
            completions[index] = finished[index].get_future();
            stoppers[index] = std::thread([&, index] {
                assert(!NativeLoop::is_any_loop_thread());
                entered[index].set_value();
                assert(start_gate.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
                transport->stop();
                // Keep these descriptors open through all remaining stop
                // calls; newly freed listener-fd numbers may now be reused.
                reused_fds[index] = std::make_unique<PipeCanary>();
                transport->stop();
                finished[index].set_value();
            });
        }
        for (auto& entry : entries) assert(ready_within(entry, std::chrono::seconds(5)));
        release_stoppers.set_value();
        for (size_t index = 0; index < kStoppers; ++index) {
            assert(ready_within(completions[index], std::chrono::seconds(5)));
            stoppers[index].join();
        }
        existing_fd.assert_intact();
        for (const auto& canary : reused_fds) {
            assert(canary);
            canary->assert_intact();
        }
        // In the never-started case, drain the start job left in the queue; it
        // must not resurrect a listener that the external callers just closed.
        runtime.loop_for(0)->poll_once(0);
        assert(!NativeLoop::is_any_loop_thread());
        assert(accepted.load() == 0);
        assert_listener_port_released(bound);
        transport->stop();
        existing_fd.assert_intact();
        for (const auto& canary : reused_fds) canary->assert_intact();
        assert(runtime.exceptions_total() == 0);
    }
}

// stop() must silence the accept callback before it returns, even when
// connections keep arriving while the close job is queued behind a drain that
// is already running on the loop thread. The connectors use a SMALL bounded
// number of attempts: a tight hammer loop would exhaust the machine's
// ephemeral port space (30s TIME_WAIT each) and poison every other loopback
// test on the host.
void stop_during_accept(PollerBackend backend, uint16_t hint) {
    for (int round = 0; round < 4; ++round) {
        NativeRuntime runtime(2, backend);
        runtime.start();
        uint16_t bound = 0;
        auto transport = make_listener(runtime, static_cast<uint16_t>(hint + 6 + round), bound);

        std::atomic<bool> stopped{false};
        std::atomic<bool> connecting{true};
        std::atomic<int> accepted{0};
        std::atomic<int> delivered_after_stop{0};
        transport->start([&](std::unique_ptr<IConnection> connection, const ListenerConfig&) {
            if (stopped.load(std::memory_order_acquire))
                delivered_after_stop.fetch_add(1, std::memory_order_relaxed);
            accepted.fetch_add(1, std::memory_order_relaxed);
            connection->close();
        });

        std::vector<std::thread> connectors;
        for (int i = 0; i < 2; ++i) {
            connectors.emplace_back([&] {
                while (connecting.load(std::memory_order_acquire)) {
                    for (int burst = 0; burst < 8; ++burst) {
                        if (!connecting.load(std::memory_order_acquire)) return;
                        const int fd = try_connect(bound);
                        if (fd >= 0) ::close(fd);
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }
                }
            });
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        transport->stop();
        stopped.store(true, std::memory_order_release);
        // Give any accept callback that was queued behind the close job time
        // to run; with the fix it is dropped at delivery.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        connecting.store(false, std::memory_order_release);
        for (auto& connector : connectors) connector.join();

        assert(delivered_after_stop.load() == 0);
        transport->stop(); // idempotent
        runtime.stop(true);
    }
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
        accepted_connections_round_robin(backend, hint);
        server_transports_share_runtime_cursor(backend, hint);
        hostname_falls_back_to_next_address(backend, hint);
        stop_waits_for_active_accept_not_queued_target(backend, hint);
        for (const bool from_grave : {false, true})
            handlers_stop_other_runtime_without_deadlock(backend, hint, from_grave);
        concurrent_external_stop_without_live_loop(backend, hint);
        stop_during_accept(backend, hint);
    }
    return 0;
}
