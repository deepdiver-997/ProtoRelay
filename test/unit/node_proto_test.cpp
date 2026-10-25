#undef NDEBUG
#include "framework/node/node_proto.h"
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
    if (const char* p = std::getenv("PROTORELAY_TEST_PORT_BASE")) return uint16_t(std::atoi(p) + 130);
    return 20330;
}
void put32(std::string& s, uint32_t n) { for (int i = 3; i >= 0; --i) s.push_back(char(n >> (i * 8))); }
void put64(std::string& s, uint64_t n) { for (int i = 7; i >= 0; --i) s.push_back(char(n >> (i * 8))); }
uint64_t get64(const char* p) { uint64_t n = 0; for (int i = 0; i < 8; ++i) n = (n << 8) | uint8_t(p[i]); return n; }
uint64_t now_ms() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count());
}

LinkOptions link_options(std::string id, std::string peer) {
    LinkOptions o;
    o.node_id = std::move(id);
    o.expected_peer_id = std::move(peer);
    o.heartbeat_interval = 40ms;
    o.heartbeat_timeout = 90ms;
    o.reconnect_min = 25ms;
    o.reconnect_max = 120ms;
    return o;
}

std::unique_ptr<IServerTransport> make_listener(NativeRuntime& runtime, uint16_t port) {
    ServerTransportConfig config;
    config.address = "127.0.0.1";
    config.listeners.emplace_back(ListenerType::TCP, port);
    return make_native_server_transport(config, {runtime.loop_for(0)});
}

// Message/error collection safe for executor callbacks.
struct Sink {
    std::mutex mutex;
    std::vector<Envelope> messages;
    std::vector<std::tuple<Envelope, MessageErrorCode, uint64_t>> errors;
    void on_message(const Envelope& e) { std::lock_guard<std::mutex> lock(mutex); messages.push_back(e); }
    void on_error(const Envelope& e, MessageErrorCode c, uint64_t id) {
        std::lock_guard<std::mutex> lock(mutex);
        errors.emplace_back(e, c, id);
    }
    size_t message_count() { std::lock_guard<std::mutex> lock(mutex); return messages.size(); }
    size_t error_count(MessageErrorCode code) {
        std::lock_guard<std::mutex> lock(mutex);
        size_t n = 0;
        for (const auto& e : errors)
            if (std::get<1>(e) == code) ++n;
        return n;
    }
    bool has_error(MessageErrorCode code, uint64_t failed_id, const char* source) {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& e : errors)
            if (std::get<1>(e) == code && std::get<2>(e) == failed_id && std::get<0>(e).source == source) return true;
        return false;
    }
};

// Linear topology a -> b -> c -> d (one NodeLink edge per arrow, client end
// at the lower node). Every router runs on the single runtime loop, so task
// ordering is deterministic: completions queued by later writes run after
// receive tasks that were queued earlier.
struct Chain {
    NativeRuntime runtime;
    std::unique_ptr<IServerTransport> lb, lc, ld;
    std::shared_ptr<NodeRouter> a, b, c, d;
    std::shared_ptr<NodeLink> ab, bc, cd, ba, cb, dc;
    std::vector<std::shared_ptr<NodeLink>> all;

    explicit Chain(PollerBackend backend, const NodeRouterOptions& opts = {},
                   const NodeRouterOptions* relay_opts = nullptr)
        : runtime(1, backend) {
        runtime.start();
        auto ex = runtime.loop_for(0);
        a = NodeRouter::create(ex, "a", opts);
        b = NodeRouter::create(ex, "b", relay_opts ? *relay_opts : opts);
        c = NodeRouter::create(ex, "c", relay_opts ? *relay_opts : opts);
        d = NodeRouter::create(ex, "d", opts);
        lb = make_listener(runtime, port_hint());
        lc = make_listener(runtime, uint16_t(port_hint() + 1));
        ld = make_listener(runtime, uint16_t(port_hint() + 2));
        ab = edge(lb, "a", "b", ba);
        bc = edge(lc, "b", "c", cb);
        cd = edge(ld, "c", "d", dc);
        a->attach("b", ab);
        b->attach("a", ba);
        b->attach("c", bc);
        c->attach("b", cb);
        c->attach("d", cd);
        d->attach("c", dc);
    }
    std::shared_ptr<NodeLink> edge(std::unique_ptr<IServerTransport>& listener,
                                   const std::string& x, const std::string& y,
                                   std::shared_ptr<NodeLink>& accepted_out) {
        std::mutex mutex;
        listener->start([&, x, y](std::unique_ptr<IConnection> conn, const ListenerConfig&) {
            auto end = NodeLink::accepted(std::move(conn), link_options(y, x));
            std::lock_guard<std::mutex> lock(mutex);
            accepted_out = end;
            all.push_back(std::move(end));
        });
        auto client = NodeLink::client(runtime.loop_for(0), link_options(x, y));
        client->start("127.0.0.1", listener->listeners().front().port);
        await([&] {
            std::lock_guard<std::mutex> lock(mutex);
            return client->ready() && accepted_out && accepted_out->ready();
        });
        all.push_back(client);
        return client;
    }
    std::error_code send_sync(const std::shared_ptr<NodeRouter>& from, Envelope m) {
        std::promise<std::error_code> done;
        auto outcome = done.get_future();
        from->send(std::move(m), [&](std::error_code ec) { done.set_value(ec); });
        assert(outcome.wait_for(3s) == std::future_status::ready);
        return outcome.get();
    }
    ~Chain() {
        for (auto& link : all) link->stop();
        lb->stop();
        lc->stop();
        ld->stop();
        runtime.stop(true);
    }
};

void codec() {
    Envelope e;
    e.type = MessageType::data;
    e.ttl = 5;
    e.nonce = 0x11223344;
    e.timestamp_ms = 0x0102030405060708ULL;
    e.message_id = 0xAABBCCDDEEFF0011ULL;
    e.source = "a";
    e.destination = "d";
    e.route = {"b", "c"};
    e.route_pos = 0;
    e.payload = "hello";
    const auto wire = encode_envelope(e);
    assert(wire.size() == 51); // 28 fixed + src + dst + route(2 hops) + 4 + payload
    assert(uint8_t(wire[0]) == 1 && uint8_t(wire[1]) == 1 && uint8_t(wire[2]) == 0 && uint8_t(wire[3]) == 0);
    assert(uint8_t(wire[7]) == 5 && uint8_t(wire[8]) == 0x11 && uint8_t(wire[11]) == 0x44);
    NodeRouterOptions limits;
    Envelope p;
    assert(decode_envelope(wire, limits, p) == DecodeResult::ok);
    assert(p.type == MessageType::data && p.ttl == 5 && p.nonce == 0x11223344);
    assert(p.timestamp_ms == 0x0102030405060708ULL && p.message_id == 0xAABBCCDDEEFF0011ULL);
    assert(p.source == "a" && p.destination == "d");
    assert(p.route.size() == 2 && p.route[0] == "b" && p.route[1] == "c" && p.route_pos == 0);
    assert(p.payload == "hello" && !p.encrypted);
    for (size_t n = 0; n < wire.size(); ++n) assert(decode_envelope(wire.substr(0, n), limits, p) != DecodeResult::ok);
    assert(decode_envelope(wire + "x", limits, p) == DecodeResult::malformed);
    auto v2 = wire;
    v2[0] = 2;
    assert(decode_envelope(v2, limits, p) == DecodeResult::unknown_version);
    auto reserved = wire;
    reserved[2] = 2;
    assert(decode_envelope(reserved, limits, p) == DecodeResult::malformed);
    auto unknown = wire;
    unknown[1] = 200;
    assert(decode_envelope(unknown, limits, p) == DecodeResult::ok_unknown_type);
    assert(p.source == "a" && p.destination == "d"); // routing fields still parse
    NodeRouterOptions short_ids;
    short_ids.max_node_id_bytes = 2;
    Envelope long_id = e;
    long_id.source = "abc"; // 3 bytes exceeds the 2-byte limit
    assert(decode_envelope(encode_envelope(long_id), short_ids, p) == DecodeResult::malformed);
    NodeRouterOptions tiny;
    tiny.max_envelope_bytes = 16;
    assert(decode_envelope(wire, tiny, p) == DecodeResult::malformed);
    bool threw = false;
    Envelope bad_pos = e;
    bad_pos.route_pos = 3;
    try { encode_envelope(bad_pos); } catch (const std::invalid_argument&) { threw = true; }
    assert(threw);
    Envelope no_dst = e;
    no_dst.destination.clear();
    threw = false;
    try { encode_envelope(no_dst); } catch (const std::invalid_argument&) { threw = true; }
    assert(threw);
    Envelope cipher = e;
    cipher.encrypted = true;
    cipher.key_id = 7;
    cipher.payload = std::string("\x01\x02\x03", 3); // codec passes ciphertext through verbatim
    assert(decode_envelope(encode_envelope(cipher), limits, p) == DecodeResult::ok);
    assert(p.encrypted && p.key_id == 7 && p.payload == std::string("\x01\x02\x03", 3));
}

void one_hop(PollerBackend backend) {
    Chain f(backend);
    Sink at_b, at_d;
    f.b->on_message([&](const Envelope& e) { at_b.on_message(e); });
    f.d->on_message([&](const Envelope& e) { at_d.on_message(e); });
    Envelope routed;
    routed.destination = "d";
    routed.route = {"b", "c"};
    routed.message_id = 1000;
    routed.payload = "end-to-end";
    assert(f.send_sync(f.a, routed) == std::error_code{});
    Envelope direct;
    direct.destination = "b";
    direct.message_id = 1001;
    direct.payload = "neighbor";
    assert(f.send_sync(f.a, direct) == std::error_code{});
    await([&] { return at_d.message_count() == 1 && at_b.message_count() == 1; });
    const auto& got = at_d.messages.front();
    assert(got.type == MessageType::data && got.source == "a" && got.destination == "d");
    assert(got.message_id == 1000 && got.payload == "end-to-end");
    assert(got.ttl == 6 && got.route_pos == 2); // max_hops 8 minus two forwarding hops
    const auto& neighbor = at_b.messages.front();
    assert(neighbor.message_id == 1001 && neighbor.payload == "neighbor");
    assert(neighbor.ttl == 8 && neighbor.route.empty()); // direct: never forwarded
}

void unknown_next_hop(PollerBackend backend) {
    Chain f(backend);
    Sink reports;
    f.a->on_error_report([&](const Envelope& e, MessageErrorCode c, uint64_t id) { reports.on_error(e, c, id); });
    Envelope m;
    m.destination = "x";
    m.route = {"b"};
    m.message_id = 2000;
    m.payload = "lost";
    assert(f.send_sync(f.a, m) == std::error_code{}); // first hop b is a neighbor
    await([&] { return reports.has_error(MessageErrorCode::unroutable, 2000, "b"); });
}

void multihop_fail_far(PollerBackend backend) {
    Chain f(backend);
    f.c->detach("d");
    f.d->detach("c");
    Sink reports;
    f.a->on_error_report([&](const Envelope& e, MessageErrorCode c, uint64_t id) { reports.on_error(e, c, id); });
    Envelope m;
    m.destination = "d";
    m.route = {"b", "c"};
    m.message_id = 2001;
    m.payload = "far";
    assert(f.send_sync(f.a, m) == std::error_code{});
    // The report originates at c and travels the reversed prefix c -> b -> a.
    await([&] { return reports.has_error(MessageErrorCode::unroutable, 2001, "c"); });
    std::lock_guard<std::mutex> lock(reports.mutex);
    assert(std::get<0>(reports.errors.front()).route == std::vector<std::string>{"b"});
}

void ttl_exhausted(PollerBackend backend) {
    NodeRouterOptions opts;
    opts.max_hops = 1;
    Chain f(backend, opts);
    Sink reports, at_d;
    f.a->on_error_report([&](const Envelope& e, MessageErrorCode c, uint64_t id) { reports.on_error(e, c, id); });
    f.d->on_message([&](const Envelope& e) { at_d.on_message(e); });
    Envelope m;
    m.destination = "d";
    m.route = {"b", "c"};
    m.message_id = 3000;
    m.payload = "too-far";
    assert(f.send_sync(f.a, m) == std::error_code{});
    await([&] { return reports.has_error(MessageErrorCode::ttl_exhausted, 3000, "c"); });
    std::this_thread::sleep_for(40ms);
    assert(at_d.message_count() == 0);
}

void loop_dedup(PollerBackend backend) {
    Chain f(backend);
    Sink at_d, reports;
    f.d->on_message([&](const Envelope& e) { at_d.on_message(e); });
    f.a->on_error_report([&](const Envelope& e, MessageErrorCode c, uint64_t id) { reports.on_error(e, c, id); });
    Envelope m;
    m.destination = "d";
    m.route = {"b", "c"};
    m.message_id = 4242;
    m.nonce = 1;
    m.timestamp_ms = now_ms();
    m.payload = "once";
    assert(f.send_sync(f.a, m) == std::error_code{});
    assert(f.send_sync(f.a, m) == std::error_code{}); // identical copy: loop/replay
    await([&] { return at_d.message_count() == 1; });
    std::this_thread::sleep_for(60ms);
    assert(at_d.message_count() == 1);  // duplicate suppressed
    assert(reports.errors.empty());     // and silent (no error storm)
}

void replay_nonce(PollerBackend backend) {
    Chain f(backend);
    Sink at_b, reports;
    f.b->on_message([&](const Envelope& e) { at_b.on_message(e); });
    f.a->on_error_report([&](const Envelope& e, MessageErrorCode c, uint64_t id) { reports.on_error(e, c, id); });
    Envelope m;
    m.destination = "b";
    m.message_id = 1;
    m.nonce = 77;
    m.timestamp_ms = now_ms();
    m.payload = "first";
    assert(f.send_sync(f.a, m) == std::error_code{});
    Envelope reused = m;
    reused.message_id = 2; // fresh id so only the nonce window can reject
    reused.payload = "second";
    assert(f.send_sync(f.a, reused) == std::error_code{});
    await([&] { return at_b.message_count() == 1 && reports.has_error(MessageErrorCode::replay_rejected, 2, "b"); });
    assert(at_b.messages.front().payload == "first");
}

void timestamp_skew(PollerBackend backend) {
    Chain f(backend);
    Sink at_b, reports;
    f.b->on_message([&](const Envelope& e) { at_b.on_message(e); });
    f.a->on_error_report([&](const Envelope& e, MessageErrorCode c, uint64_t id) { reports.on_error(e, c, id); });
    Envelope stale;
    stale.destination = "b";
    stale.message_id = 5;
    stale.nonce = 9;
    stale.timestamp_ms = now_ms() - 120000; // 2 min old, tolerance 30 s
    stale.payload = "stale";
    assert(f.send_sync(f.a, stale) == std::error_code{});
    Envelope future;
    future.destination = "b";
    future.message_id = 6;
    future.nonce = 10;
    future.timestamp_ms = now_ms() + 120000;
    future.payload = "future";
    assert(f.send_sync(f.a, future) == std::error_code{});
    await([&] { return reports.error_count(MessageErrorCode::replay_rejected) == 2; });
    assert(reports.has_error(MessageErrorCode::replay_rejected, 5, "b"));
    assert(reports.has_error(MessageErrorCode::replay_rejected, 6, "b"));
    std::this_thread::sleep_for(40ms);
    assert(at_b.message_count() == 0);
}

#ifdef PR_NODE_PROTO_TEST_AEAD
void encrypted_e2e(PollerBackend backend) {
    const std::string key1(32, '\x5a');
    const std::string key2(32, '\x5b');
    {
        Chain f(backend);
        // Only the endpoints hold keys: the relays b and c forward ciphertext
        // they cannot read (and never see a delivery callback).
        f.a->add_key(1, key1);
        f.d->add_key(1, key1);
        Sink at_d, at_relay;
        f.d->on_message([&](const Envelope& e) { at_d.on_message(e); });
        f.b->on_message([&](const Envelope& e) { at_relay.on_message(e); });
        f.c->on_message([&](const Envelope& e) { at_relay.on_message(e); });
        Envelope m;
        m.destination = "d";
        m.route = {"b", "c"};
        m.encrypted = true;
        m.key_id = 1;
        m.message_id = 4000;
        m.payload = "secret-bytes";
        assert(f.send_sync(f.a, m) == std::error_code{});
        await([&] { return at_d.message_count() == 1; });
        assert(at_d.messages.front().payload == "secret-bytes");
        std::this_thread::sleep_for(40ms);
        assert(at_relay.message_count() == 0);
    }
    {
        Chain f(backend); // destination holds a different key: tag mismatch
        f.a->add_key(2, key1);
        f.d->add_key(2, key2);
        Sink reports;
        f.a->on_error_report([&](const Envelope& e, MessageErrorCode c, uint64_t id) { reports.on_error(e, c, id); });
        Envelope m;
        m.destination = "d";
        m.route = {"b", "c"};
        m.encrypted = true;
        m.key_id = 2;
        m.message_id = 4001;
        m.payload = "wrong-key";
        assert(f.send_sync(f.a, m) == std::error_code{});
        await([&] { return reports.has_error(MessageErrorCode::decrypt_failed, 4001, "d"); });
    }
    {
        Chain f(backend); // destination has no key at all
        f.a->add_key(3, key1);
        Sink reports;
        f.a->on_error_report([&](const Envelope& e, MessageErrorCode c, uint64_t id) { reports.on_error(e, c, id); });
        Envelope m;
        m.destination = "d";
        m.route = {"b", "c"};
        m.encrypted = true;
        m.key_id = 3;
        m.message_id = 4002;
        m.payload = "no-key";
        assert(f.send_sync(f.a, m) == std::error_code{});
        await([&] { return reports.has_error(MessageErrorCode::decrypt_failed, 4002, "d"); });
    }
}
#endif

void unknown_version_wire(PollerBackend backend) {
    Chain f(backend);
    Sink at_b, reports;
    f.b->on_message([&](const Envelope& e) { at_b.on_message(e); });
    f.a->on_error_report([&](const Envelope& e, MessageErrorCode c, uint64_t id) { reports.on_error(e, c, id); });
    Envelope m;
    m.source = "a";
    m.destination = "b";
    m.message_id = 5000;
    m.payload = "v9";
    auto bad = encode_envelope(m);
    bad[0] = 9; // unknown version: unknown layout -> silent drop, link stays up
    f.ab->notify(bad);
    Envelope after;
    after.destination = "b";
    after.message_id = 5001;
    after.payload = "after";
    assert(f.send_sync(f.a, after) == std::error_code{});
    await([&] { return at_b.message_count() == 1; });
    assert(at_b.messages.front().payload == "after");
    std::this_thread::sleep_for(40ms);
    assert(reports.errors.empty() && f.ab->ready());
}

void unknown_type_wire(PollerBackend backend) {
    Chain f(backend);
    Sink at_b, reports;
    f.b->on_message([&](const Envelope& e) { at_b.on_message(e); });
    f.a->on_error_report([&](const Envelope& e, MessageErrorCode c, uint64_t id) { reports.on_error(e, c, id); });
    Envelope m;
    m.source = "a";
    m.destination = "b";
    m.message_id = 5002;
    m.payload = "type-x";
    auto bad = encode_envelope(m);
    bad[1] = 200; // known version, unregistered type: drop + report to source
    f.ab->notify(bad);
    await([&] { return reports.has_error(MessageErrorCode::unknown_type, 5002, "b"); });
    Envelope after;
    after.destination = "b";
    after.message_id = 5003;
    after.payload = "after";
    assert(f.send_sync(f.a, after) == std::error_code{});
    await([&] { return at_b.message_count() == 1; });
    assert(f.ab->ready());
}

void send_queue_bound(PollerBackend backend) {
    NodeRouterOptions opts;
    opts.max_in_flight = 2;
    Chain f(backend, opts);
    Sink at_b;
    f.b->on_message([&](const Envelope& e) { at_b.on_message(e); });
    std::vector<std::promise<std::error_code>> results(5);
    std::vector<std::future<std::error_code>> futures;
    futures.reserve(results.size());
    for (auto& r : results) futures.push_back(r.get_future());
    // One loop task: the bound for later sends must trip before any flush
    // completion can release earlier slots (same idiom as node_link_test).
    f.runtime.loop_for(0)->post([&] {
        for (size_t i = 0; i < results.size(); ++i) {
            Envelope m;
            m.destination = "b";
            m.message_id = 6000 + i;
            m.payload = "flood";
            f.a->send(m, [&results, i](std::error_code ec) { results[i].set_value(ec); });
        }
    });
    size_t accepted = 0, rejected = 0;
    for (auto& fut : futures) {
        assert(fut.wait_for(3s) == std::future_status::ready);
        const auto ec = fut.get();
        if (ec) { assert(ec == std::errc::no_buffer_space); ++rejected; } else ++accepted;
    }
    assert(accepted == 2 && rejected == 3);
    await([&] { return f.a->in_flight() == 0; });
    await([&] { return at_b.message_count() == 2; });
}

void forward_overflow_report(PollerBackend backend) {
    NodeRouterOptions endpoint_opts;
    NodeRouterOptions relay_opts;
    relay_opts.max_in_flight = 1;
    Chain f(backend, endpoint_opts, &relay_opts);
    Sink at_c, reports;
    f.c->on_message([&](const Envelope& e) { at_c.on_message(e); });
    f.a->on_error_report([&](const Envelope& e, MessageErrorCode c, uint64_t id) { reports.on_error(e, c, id); });
    std::vector<std::promise<std::error_code>> results(4);
    std::vector<std::future<std::error_code>> futures;
    futures.reserve(results.size());
    for (auto& r : results) futures.push_back(r.get_future());
    // Single-task issuance: b forwards the first message (slot taken), and
    // later receive tasks hit the bound, so their queue_overflow reports
    // reach a.
    f.runtime.loop_for(0)->post([&] {
        for (size_t i = 0; i < results.size(); ++i) {
            Envelope m;
            m.destination = "c";
            m.route = {"b"};
            m.message_id = 7000 + i;
            m.payload = "overflow";
            f.a->send(m, [&results, i](std::error_code ec) { results[i].set_value(ec); });
        }
    });
    await([&] { return reports.error_count(MessageErrorCode::queue_overflow) >= 1; });
    for (auto& fut : futures) assert(fut.wait_for(3s) == std::future_status::ready);
    await([&] { return f.b->in_flight() == 0 && f.a->in_flight() == 0; });
    std::this_thread::sleep_for(40ms);
    // The relay bound dropped at least one message and its report reached the
    // source. The exact delivery count depends on how loopback write
    // completions interleave with the burst (1..N all respect the bound);
    // the deterministic bound assertion lives in send_queue_bound.
    assert(at_c.message_count() >= 1 && at_c.message_count() <= 4);
}

void probe_ack(PollerBackend backend) {
    Chain f(backend);
    Sink at_a;
    f.a->on_message([&](const Envelope& e) { at_a.on_message(e); });
    Envelope probe;
    probe.type = MessageType::probe;
    probe.destination = "d";
    probe.route = {"b", "c"};
    probe.message_id = 8000;
    probe.payload = "echo-me";
    assert(f.send_sync(f.a, probe) == std::error_code{});
    await([&] { return at_a.message_count() == 1; });
    const auto& ack = at_a.messages.front();
    assert(ack.type == MessageType::ack && ack.source == "d");
    assert(ack.payload.size() == 8 + probe.payload.size());
    assert(get64(ack.payload.data()) == 8000 && ack.payload.substr(8) == "echo-me");
}

void lifecycle(PollerBackend backend) {
    Chain f(backend);
    Sink at_b;
    f.b->on_message([&](const Envelope& e) { at_b.on_message(e); });
    f.a->stop();
    f.a->stop(); // idempotent
    Envelope m;
    m.destination = "b";
    m.payload = "late";
    std::promise<std::error_code> done;
    auto outcome = done.get_future();
    f.a->send(m, [&](std::error_code ec) { done.set_value(ec); });
    assert(outcome.wait_for(3s) == std::future_status::ready);
    assert(outcome.get() == std::errc::operation_canceled);
    // The peer link is still up: handoffs to a stopped node's id fail fast,
    // and frames toward a stopped router are dropped without crashing.
    Envelope to_a;
    to_a.destination = "a";
    to_a.payload = "gone";
    assert(f.send_sync(f.b, to_a) == std::error_code{});
    f.b->stop();
    Envelope to_b;
    to_b.destination = "b";
    to_b.payload = "dark";
    assert(f.send_sync(f.c, to_b) == std::error_code{});
    std::this_thread::sleep_for(40ms);
    assert(at_b.message_count() == 0);
    assert(f.a->in_flight() == 0 && f.b->in_flight() == 0);
}
} // namespace

int main() {
    codec();
    for (auto backend : available_backends()) {
        one_hop(backend);
        unknown_next_hop(backend);
        multihop_fail_far(backend);
        ttl_exhausted(backend);
        loop_dedup(backend);
        replay_nonce(backend);
        timestamp_skew(backend);
#ifdef PR_NODE_PROTO_TEST_AEAD
        encrypted_e2e(backend);
#endif
        unknown_version_wire(backend);
        unknown_type_wire(backend);
        send_queue_bound(backend);
        forward_overflow_report(backend);
        probe_ack(backend);
        lifecycle(backend);
    }
}
