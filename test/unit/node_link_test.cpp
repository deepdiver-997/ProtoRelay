#undef NDEBUG
#include "framework/node/node_link.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

using namespace pr;
using namespace pr::net;
using namespace pr::net::native;
using namespace pr::node;
using namespace std::chrono_literals;

namespace {
template<class F> void await(F predicate, std::chrono::milliseconds limit = 3s) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!predicate() && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(2ms);
    assert(predicate());
}
uint16_t port_hint() {
    if (const char* p = std::getenv("PROTORELAY_TEST_PORT_BASE")) return uint16_t(std::atoi(p) + 80);
    return 20280;
}
LinkOptions options(std::string id, std::string expected = {}) {
    LinkOptions o;
    o.node_id = std::move(id); o.expected_peer_id = std::move(expected);
    o.heartbeat_interval = 35ms; o.heartbeat_timeout = 65ms;
    o.reconnect_min = 20ms; o.reconnect_max = 100ms;
    return o;
}
struct Fixture {
    NativeRuntime runtime;
    std::unique_ptr<IServerTransport> listener;
    std::shared_ptr<NodeLink> client;
    std::mutex mutex;
    std::vector<std::shared_ptr<NodeLink>> servers;
    uint16_t port = 0;
    explicit Fixture(PollerBackend backend, LinkOptions client_options = options("client", "server"),
                     LinkOptions server_options = options("server", "client")) : runtime(1, backend) {
        runtime.start();
        ServerTransportConfig config;
        config.address = "127.0.0.1";
        config.listeners.emplace_back(ListenerType::TCP, port_hint());
        listener = make_native_server_transport(config, {runtime.loop_for(0)});
        port = listener->listeners().front().port;
        listener->start([this, server_options](std::unique_ptr<IConnection> connection, const ListenerConfig&) {
            auto server = NodeLink::accepted(std::move(connection), server_options);
            server->on_request([weak = std::weak_ptr<NodeLink>(server)](uint64_t id, const std::string& payload) {
                if (auto link = weak.lock()) if (payload != "ignore") link->respond(id, payload);
            });
            std::lock_guard<std::mutex> lock(mutex); servers.push_back(std::move(server));
        });
        client = NodeLink::client(runtime.loop_for(0), client_options);
        client->start("127.0.0.1", port);
    }
    std::shared_ptr<NodeLink> server(size_t index = 0) {
        std::lock_guard<std::mutex> lock(mutex);
        return index < servers.size() ? servers[index] : nullptr;
    }
    ~Fixture() {
        client->stop();
        { std::lock_guard<std::mutex> lock(mutex); for (auto& server : servers) server->stop(); }
        listener->stop();
        runtime.stop(true);
    }
};
void codec() {
    const auto wire = encode_frame({FrameType::request, 0x0102030405060708ULL, std::string("a\0b", 3)});
    assert(wire.size() == 17);
    assert(uint8_t(wire[0]) == 0 && uint8_t(wire[3]) == 13);
    Frame f{};
    assert(decode_frame(wire.substr(4), 13, f));
    assert(f.id == 0x0102030405060708ULL && f.payload == std::string("a\0b", 3));
    assert(!decode_frame(wire.substr(4, 9), 13, f));
    assert(!decode_frame(wire.substr(4), 12, f));
    auto bad = wire.substr(4); bad[0] = 2;
    assert(!decode_frame(bad, 13, f));
}
void roundtrip(PollerBackend backend) {
    Fixture f(backend);
    await([&] { return f.client->ready() && f.server() && f.server()->ready(); });
    assert(f.client->peer_id() == "server" && f.server()->peer_id() == "client");
    std::promise<std::string> p1, p2;
    auto a = p1.get_future(), b = p2.get_future();
    f.client->request("first", 1s, [&p1](std::error_code error, std::string result) {
        assert(!error); p1.set_value(std::move(result));
    });
    f.client->request("second", 1s, [&p2](std::error_code error, std::string result) {
        assert(!error); p2.set_value(std::move(result));
    });
    assert(a.wait_for(3s) == std::future_status::ready && a.get() == "first");
    assert(b.wait_for(3s) == std::future_status::ready && b.get() == "second");
    std::promise<std::string> notified;
    auto notification = notified.get_future();
    f.server()->on_notify([&notified](uint64_t, const std::string& payload) { notified.set_value(payload); });
    f.client->notify("one-way");
    assert(notification.wait_for(3s) == std::future_status::ready && notification.get() == "one-way");
    std::atomic<int> timeout_count{0}, cancel_count{0};
    std::promise<void> timed, canceled;
    auto tf = timed.get_future(), cf = canceled.get_future();
    f.client->request("ignore", 45ms, [&](std::error_code error, std::string) {
        assert(error == std::errc::timed_out); ++timeout_count; timed.set_value();
    });
    auto id = f.client->request("ignore", 1s, [&](std::error_code error, std::string) {
        assert(error == std::errc::operation_canceled); ++cancel_count; canceled.set_value();
    });
    f.client->cancel(id);
    assert(tf.wait_for(3s) == std::future_status::ready);
    assert(cf.wait_for(3s) == std::future_status::ready);
    std::this_thread::sleep_for(80ms);
    assert(timeout_count == 1 && cancel_count == 1 && f.client->ready());
    auto attempts = f.client->connect_attempts();
    f.server()->stop();
    await([&] { return f.client->connect_attempts() > attempts && f.client->ready() && f.server(1) && f.server(1)->ready(); });
    f.client->stop();
    auto stopped_attempts = f.client->connect_attempts();
    f.server(1)->stop();
    std::this_thread::sleep_for(180ms);
    assert(f.client->connect_attempts() == stopped_attempts);
}
void rejected(PollerBackend backend) {
    Fixture f(backend, options("client", "wrong"));
    await([&] { return f.client->connect_attempts() >= 2; });
    assert(!f.client->ready());
}
void server_rejected(PollerBackend backend) {
    Fixture f(backend, options("client", "server"), options("server", "wrong"));
    await([&] { return f.client->connect_attempts() >= 2; });
    assert(f.server() && !f.server()->ready());
}
void bounded(PollerBackend backend) {
    auto client = options("client", "server");
    client.max_pending_requests = 1;
    Fixture f(backend, client);
    await([&] { return f.client->ready() && f.server() && f.server()->ready(); });
    std::this_thread::sleep_for(10ms);
    std::promise<std::error_code> first, second;
    auto a = first.get_future(), b = second.get_future();
    f.client->request("ignore", 1s, [&](std::error_code e, std::string) { first.set_value(e); });
    f.client->request("ignore", 1s, [&](std::error_code e, std::string) { second.set_value(e); });
    assert(b.wait_for(3s) == std::future_status::ready && b.get() == std::errc::no_buffer_space);
    f.client->stop();
    assert(a.wait_for(3s) == std::future_status::ready && a.get() == std::errc::operation_canceled);
}
void queue_bound(PollerBackend backend) {
    auto client = options("client", "server");
    client.max_queued_frames = 1;
    Fixture f(backend, client);
    await([&] { return f.client->ready() && f.server() && f.server()->ready(); });
    std::this_thread::sleep_for(10ms);
    std::promise<std::error_code> first, second;
    auto a = first.get_future(), b = second.get_future();
    f.client->request("ignore", 1s, [&](std::error_code e, std::string) { first.set_value(e); });
    f.client->request("ignore", 1s, [&](std::error_code e, std::string) { second.set_value(e); });
    assert(b.wait_for(3s) == std::future_status::ready && b.get() == std::errc::no_buffer_space);
    f.client->stop();
    assert(a.wait_for(3s) == std::future_status::ready && a.get() == std::errc::operation_canceled);
}
void heartbeat_timeout(PollerBackend backend) {
    NativeRuntime runtime(1, backend); runtime.start();
    ServerTransportConfig config; config.address = "127.0.0.1";
    config.listeners.emplace_back(ListenerType::TCP, port_hint());
    auto listener = make_native_server_transport(config, {runtime.loop_for(0)});
    std::vector<std::unique_ptr<IConnection>> silent;
    listener->start([&silent](std::unique_ptr<IConnection> conn, const ListenerConfig&) { silent.push_back(std::move(conn)); });
    auto client = NodeLink::client(runtime.loop_for(0), options("client", "server"));
    client->start("127.0.0.1", listener->listeners().front().port);
    await([&] { return client->connect_attempts() >= 2; });
    assert(!client->ready());
    client->stop(); listener->stop(); runtime.stop(true);
}
void pong_timeout(PollerBackend backend) {
    NativeRuntime runtime(1, backend); runtime.start();
    ServerTransportConfig config; config.address = "127.0.0.1";
    config.listeners.emplace_back(ListenerType::TCP, port_hint());
    auto listener = make_native_server_transport(config, {runtime.loop_for(0)});
    std::shared_ptr<NodeLink> server;
    std::atomic<bool> ready{false}, timed_out{false};
    listener->start([&](std::unique_ptr<IConnection> conn, const ListenerConfig&) {
        server = NodeLink::accepted(std::move(conn), options("server", "client"));
        server->on_state([&](bool state, std::error_code error) {
            if (state) ready = true;
            else if (error == std::errc::timed_out) timed_out = true;
        });
    });
    auto peer = runtime.make_client_transport();
    std::promise<void> sent; auto result = sent.get_future();
    peer->async_connect("127.0.0.1", listener->listeners().front().port,
        [peer, &sent](std::error_code error) {
            assert(!error);
            auto hello = std::make_shared<std::string>(encode_frame({FrameType::hello, 0, "client"}));
            peer->async_write(net::buffer(*hello), [hello, &sent](std::error_code error, size_t) {
                assert(!error); sent.set_value();
            });
        });
    assert(result.wait_for(3s) == std::future_status::ready);
    await([&] { return ready.load(); });
    await([&] { return timed_out.load(); });
    peer->executor()->post([peer] { peer->close(); });
    listener->stop(); server->stop(); runtime.stop(true);
}
void malformed(PollerBackend backend) {
    NativeRuntime runtime(1, backend); runtime.start();
    ServerTransportConfig config; config.address = "127.0.0.1";
    config.listeners.emplace_back(ListenerType::TCP, port_hint());
    auto listener = make_native_server_transport(config, {runtime.loop_for(0)});
    std::atomic<int> errors{0}; std::vector<std::shared_ptr<NodeLink>> links;
    listener->start([&](std::unique_ptr<IConnection> conn, const ListenerConfig&) {
        auto link = NodeLink::accepted(std::move(conn), options("server"));
        link->on_state([&](bool ready, std::error_code error) { if (!ready && error) ++errors; });
        links.push_back(link);
    });
    auto send_bad = [&](std::string bytes) {
        auto peer = runtime.make_client_transport();
        std::promise<void> done; auto outcome = done.get_future();
        peer->async_connect("127.0.0.1", listener->listeners().front().port,
            [peer, bytes = std::move(bytes), &done](std::error_code error) {
                assert(!error);
                auto storage = std::make_shared<std::string>(bytes);
                peer->async_write(net::buffer(*storage), [peer, storage, &done](std::error_code error, size_t) {
                    assert(!error); done.set_value(); peer->close();
                });
            });
        assert(outcome.wait_for(3s) == std::future_status::ready);
    };
    send_bad(std::string("\0\0\0\1", 4)); // invalid short length
    send_bad(std::string("\x7f\xff\xff\xff", 4)); // oversize
    auto partial = encode_frame({FrameType::hello, 0, "peer"}); partial.pop_back();
    send_bad(partial); // truncated by EOF
    await([&] { return errors >= 3; });
    listener->stop();
    for (auto& link : links) link->stop();
    runtime.stop(true);
}
}
int main() {
    codec();
    for (auto backend : available_backends()) {
        roundtrip(backend);
        rejected(backend);
        server_rejected(backend);
        bounded(backend);
        queue_bound(backend);
        heartbeat_timeout(backend);
        pong_timeout(backend);
        malformed(backend);
    }
}
