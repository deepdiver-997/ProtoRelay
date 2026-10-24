// Node link v1 acceptance: signed-challenge mutual authentication (Ed25519),
// replay/forgery/outsider rejection, controller-signed epoch config (apply,
// stale-ignore, bad-signature-ignore, revocation cut-off, key rotation),
// TLS-encrypted links with plaintext-downgrade refusal, and key-file hygiene.
// Transport scenarios run on every compiled poller backend; scripted raw
// peers speak the wire format directly so handshake conformance and replay
// resistance are checked at the byte level. Keys and certificates are
// generated in-process; files land in a process-exclusive temp directory
// (AGENT_TMP_ROOT) and no real key material ships with the repo.
#undef NDEBUG
#include "framework/node/node_auth.h"
#include "framework/node/node_link.h"
#include "framework/net/native_transport.h"
#ifdef NODE_AUTH_TEST_HAVE_NATIVE_TLS
#include "framework/net/native_tls.h"
#endif

#include <sys/stat.h>
#include <unistd.h>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cassert>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using namespace pr;
using namespace pr::net;
using namespace pr::net::native;
using namespace pr::node;
using namespace pr::node::auth;
using namespace std::chrono_literals;

namespace {

template <typename F> void await(F predicate, std::chrono::milliseconds limit = 3s) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!predicate() && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(2ms);
    assert(predicate());
}
uint16_t port_hint() {
    if (const char* p = std::getenv("PROTORELAY_TEST_PORT_BASE")) return uint16_t(std::atoi(p) + 90);
    return 20290;
}

// ---- process-exclusive temp directory (AGENT_TMP_ROOT when set) ----

struct TmpDir {
    std::string path;
    TmpDir() {
        const char* configured = std::getenv("AGENT_TMP_ROOT");
        const std::string root =
            configured && *configured ? std::string(configured) : std::string("/tmp");
        std::filesystem::create_directories(root);
        std::string pattern = root + "/node_auth_test_XXXXXX";
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        assert(::mkdtemp(buffer.data()) != nullptr);
        path = buffer.data();
    }
    ~TmpDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    std::string file(const char* name) const { return path + "/" + name; }
};

void write_file(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    assert(out.write(content.data(), static_cast<std::streamsize>(content.size())));
}
void chmod_file(const std::string& path, ::mode_t mode) { assert(::chmod(path.c_str(), mode) == 0); }

template <typename F> bool throws_runtime_error(F&& action) {
    try {
        action();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

// ---- test cluster: controller plus node keys, per-node trust stores ----

struct Cluster {
    Ed25519KeyPair controller;
    std::map<std::string, Ed25519KeyPair> keys;
    Cluster() {
        controller = generate_ed25519_key_pair();
        for (const char* id : {"a", "b", "c", "eve"}) keys[id] = generate_ed25519_key_pair();
    }
    // Store for `viewer` at the given epoch trusting exactly `neighbors`.
    std::shared_ptr<TrustStore> store(uint64_t epoch, const std::vector<std::string>& neighbors) const {
        TrustStore::Snapshot snapshot;
        snapshot.epoch = epoch;
        for (const auto& id : neighbors) snapshot.neighbors.push_back({id, keys.at(id).public_key});
        return std::make_shared<TrustStore>(controller.public_key, std::move(snapshot));
    }
};

LinkOptions auth_options(const Cluster& cluster, const std::string& id,
                         std::shared_ptr<TrustStore> trust, std::string expected = {},
                         std::chrono::milliseconds heartbeat_timeout = 65ms) {
    LinkOptions o;
    o.node_id = id;
    o.expected_peer_id = std::move(expected);
    o.heartbeat_interval = 35ms;
    o.heartbeat_timeout = heartbeat_timeout;
    o.reconnect_min = 20ms;
    o.reconnect_max = 120ms;
    o.trust = std::move(trust);
    o.credentials = make_credentials(cluster.keys.at(id));
    return o;
}

// Controller-signed epoch-config payload (canonical bytes + trailing sig).
std::string signed_epoch_update(const Cluster& cluster, uint64_t epoch,
                                const std::vector<TrustStore::Neighbor>& neighbors,
                                const std::vector<std::string>& revoked,
                                const Ed25519KeyPair* signer = nullptr) {
    auto payload = encode_epoch_update(epoch, neighbors, revoked);
    payload += sign(signer ? signer->private_key_pem : cluster.controller.private_key_pem, payload);
    return payload;
}

// ---- server listener shared by the transport scenarios ----

struct ServerPool {
    std::mutex mutex;
    std::vector<std::shared_ptr<NodeLink>> links;
    std::vector<std::error_code> errors;
    std::shared_ptr<NodeLink> link(size_t index) {
        std::lock_guard<std::mutex> lock(mutex);
        return index < links.size() ? links[index] : nullptr;
    }
    bool any_ready() {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& link : links)
            if (link->ready()) return true;
        return false;
    }
    size_t error_count(std::errc code) {
        std::lock_guard<std::mutex> lock(mutex);
        size_t n = 0;
        for (const auto& error : errors)
            if (error == std::make_error_code(code)) ++n;
        return n;
    }
};

struct ListenerHandle {
    NativeRuntime& runtime;
    std::unique_ptr<IServerTransport> transport;
    ServerPool* pool = nullptr;
    uint16_t port = 0;
    void stop() {
        if (pool) {
            std::lock_guard<std::mutex> lock(pool->mutex);
            for (auto& link : pool->links) link->stop();
        }
        transport->stop();
    }
};

// Plain-TCP (or SSL) listener turning every accept into an authenticated
// NodeLink built from freshly-evaluated per-accept options.
ListenerHandle listen_on(NativeRuntime& runtime, ServerPool& pool, LinkOptions server_options,
                         uint16_t offset = 0, bool ssl = false, const TlsConfig* tls = nullptr) {
    ServerTransportConfig config;
    config.address = "127.0.0.1";
    config.listeners.emplace_back(ssl ? ListenerType::SSL : ListenerType::TCP,
                                  uint16_t(port_hint() + offset));
    if (tls) config.tls = *tls;
    ListenerHandle handle{runtime, make_native_server_transport(config, {runtime.loop_for(0)}), &pool};
    handle.port = handle.transport->listeners().front().port;
    handle.transport->start([&pool, server_options, ssl](std::unique_ptr<IConnection> connection,
                                                         const ListenerConfig&) {
        auto make_link = [&pool, server_options](std::unique_ptr<IConnection> connection) {
            auto server = NodeLink::accepted(std::move(connection), server_options);
            server->on_request([weak = std::weak_ptr<NodeLink>(server)](uint64_t id, const std::string& payload) {
                if (auto link = weak.lock()) if (payload != "ignore") link->respond(id, payload);
            });
            server->on_state([&pool](bool ready, std::error_code error) {
                if (!ready && error) {
                    std::lock_guard<std::mutex> lock(pool.mutex);
                    pool.errors.push_back(error);
                }
            });
            std::lock_guard<std::mutex> lock(pool.mutex);
            pool.links.push_back(std::move(server));
        };
        if (!ssl) {
            make_link(std::move(connection));
            return;
        }
        // SSL listener: the accept yields a TLS-wrapped connection whose
        // server-side handshake the acceptor must drive before frames flow.
        struct Handoff {
            std::unique_ptr<IConnection> connection;
            ServerPool* pool;
            LinkOptions options;
        };
        auto handoff = std::make_shared<Handoff>(Handoff{std::move(connection), &pool, server_options});
        handoff->connection->executor()->post([handoff] {
            handoff->connection->async_handshake(HandshakeRole::server,
                                                 [handoff](const std::error_code& error) {
                if (error) { handoff->connection->close(); return; }
                auto server = NodeLink::accepted(std::move(handoff->connection), std::move(handoff->options));
                server->on_request([weak = std::weak_ptr<NodeLink>(server)](uint64_t id, const std::string& payload) {
                    if (auto link = weak.lock()) if (payload != "ignore") link->respond(id, payload);
                });
                server->on_state([pool = handoff->pool](bool ready, std::error_code error) {
                    if (!ready && error) {
                        std::lock_guard<std::mutex> lock(pool->mutex);
                        pool->errors.push_back(error);
                    }
                });
                std::lock_guard<std::mutex> lock(handoff->pool->mutex);
                handoff->pool->links.push_back(std::move(server));
            });
        });
    });
    return handle;
}

// ---- scripted raw peer speaking the wire format directly ----
// These helpers mirror the handshake payload encoding on purpose: they are
// the byte-level conformance/replay half of the test.

void put32(std::string& s, uint32_t n) { for (int i = 3; i >= 0; --i) s.push_back(char(n >> (i * 8))); }
void put64(std::string& s, uint64_t n) { for (int i = 7; i >= 0; --i) s.push_back(char(n >> (i * 8))); }
uint32_t read32(const char* p) { uint32_t n = 0; for (int i = 0; i < 4; ++i) n = (n << 8) | uint8_t(p[i]); return n; }
uint64_t read64(const char* p) { uint64_t n = 0; for (int i = 0; i < 8; ++i) n = (n << 8) | uint8_t(p[i]); return n; }

std::string wire_frame(FrameType type, const std::string& payload) {
    std::string out;
    put32(out, uint32_t(10 + payload.size()));
    out.push_back(1);
    out.push_back(char(type));
    put64(out, 0);
    out += payload;
    return out;
}
bool parse_wire_frame(const std::string& wire, FrameType& type, std::string& payload) {
    // `wire` is the full on-wire frame: 4-byte length prefix, version, type,
    // 8-byte request id, payload.
    if (wire.size() < 14) return false;
    type = static_cast<FrameType>(uint8_t(wire[5]));
    payload = wire.substr(14);
    return true;
}
std::string wire_challenge(const std::string& node_id, uint64_t epoch, const std::string& nonce) {
    std::string out;
    out.push_back(1);
    put32(out, uint32_t(node_id.size()));
    out += node_id;
    put64(out, epoch);
    out += nonce;
    return out;
}
bool parse_wire_challenge(const std::string& payload, std::string& node_id, uint64_t& epoch, std::string& nonce) {
    if (payload.size() < 1 + 4) return false;
    size_t pos = 1;
    const uint32_t id_len = read32(payload.data() + pos);
    pos += 4;
    if (id_len > payload.size() - pos) return false;
    node_id = payload.substr(pos, id_len);
    pos += id_len;
    if (payload.size() - pos < 8 + 32) return false;
    epoch = read64(payload.data() + pos);
    pos += 8;
    nonce = payload.substr(pos, 32);
    return true;
}
std::string wire_auth(const std::string& node_id, uint64_t epoch, const std::string& signature) {
    std::string out;
    out.push_back(1);
    put32(out, uint32_t(node_id.size()));
    out += node_id;
    put64(out, epoch);
    out += signature;
    return out;
}
// Same canonical binding the link layer signs: identities ordered by id.
std::string wire_auth_message(const std::string& a_id, const std::string& a_nonce,
                              const std::string& b_id, const std::string& b_nonce) {
    std::string x = a_id, y = b_id;
    const std::string* x_nonce = &a_nonce;
    const std::string* y_nonce = &b_nonce;
    if (y < x) { std::swap(x, y); std::swap(x_nonce, y_nonce); }
    std::string out = "protorelay/node-auth/v1";
    put32(out, uint32_t(x.size())); out += x; out += *x_nonce;
    put32(out, uint32_t(y.size())); out += y; out += *y_nonce;
    return out;
}

// Blocking one-wire-frame read from a raw connection (test thread). Returns
// nullopt on EOF/error/timeout; on timeout the caller must close the
// connection (the abandoned read completes into shared state).
std::optional<std::string> read_one_wire(IConnection& connection, std::chrono::milliseconds budget = 1500ms) {
    std::string buffer;
    for (;;) {
        if (buffer.size() >= 4) {
            const uint32_t length = read32(buffer.data());
            if (length < 10 || length > 1024 * 1024) return std::nullopt;
            if (buffer.size() >= size_t(length) + 4) return buffer.substr(0, size_t(length) + 4);
        }
        struct ReadState {
            std::promise<bool> done;
            std::vector<char> chunk = std::vector<char>(4096);
            std::string data;
            bool ok = false;
        };
        auto state = std::make_shared<ReadState>();
        connection.async_read(net::buffer(state->chunk), [state](std::error_code error, size_t n) {
            state->data.assign(state->chunk.data(), n);
            state->ok = !error && n > 0;
            state->done.set_value(true);
        });
        auto outcome = state->done.get_future();
        if (outcome.wait_for(budget) != std::future_status::ready) return std::nullopt;
        outcome.get();
        if (!state->ok) return std::nullopt;
        buffer += state->data;
    }
}
bool write_wire(IConnection& connection, const std::string& wire) {
    std::promise<bool> done;
    auto storage = std::make_shared<std::string>(wire);
    connection.async_write(net::buffer(*storage), [storage, &done](std::error_code error, size_t) {
        done.set_value(!error);
    });
    auto outcome = done.get_future();
    return outcome.wait_for(3s) == std::future_status::ready && outcome.get();
}
void close_raw(const std::shared_ptr<IConnection>& connection) {
    if (!connection) return;
    connection->executor()->post([connection] { connection->close(); });
}

// Scripted server-side handshake completion: reads the client's challenge,
// answers with a challenge carrying server_nonce plus a genuine signature.
bool scripted_server_handshake(IConnection& connection, const Cluster& cluster,
                               const std::string& server_id, const Ed25519KeyPair& server_key,
                               uint64_t epoch, const std::string& server_nonce) {
    const auto wire = read_one_wire(connection);
    if (!wire) return false;
    FrameType type = FrameType::hello;
    std::string payload;
    if (!parse_wire_frame(*wire, type, payload) || type != FrameType::hello) return false;
    std::string client_id, client_nonce;
    uint64_t client_epoch = 0;
    if (!parse_wire_challenge(payload, client_id, client_epoch, client_nonce)) return false;
    const auto server_sig = sign(server_key.private_key_pem,
                                 wire_auth_message(server_id, server_nonce, client_id, client_nonce));
    return write_wire(connection,
                      wire_frame(FrameType::hello, wire_challenge(server_id, epoch, server_nonce)) +
                      wire_frame(FrameType::hello_auth, wire_auth(server_id, epoch, server_sig)));
}

class ScriptedPeer {
public:
    std::shared_ptr<IClientTransport> conn;
    void connect(NativeRuntime& runtime, uint16_t port) {
        conn = runtime.make_client_transport();
        std::promise<std::error_code> done;
        conn->async_connect("127.0.0.1", port, [this, &done](std::error_code error) { done.set_value(error); });
        assert(done.get_future().wait_for(3s) == std::future_status::ready);
    }
    std::optional<std::string> read_wire(std::chrono::milliseconds budget = 1500ms) {
        const auto wire = read_one_wire(*conn, budget);
        if (!wire) close();
        return wire;
    }
    void send(const std::string& wire) { assert(write_wire(*conn, wire)); }
    void close() { close_raw(conn); }
};

// ---- key hygiene (no transport) ----

void credential_hygiene() {
    Cluster cluster;
    TmpDir dir;
    const auto path = dir.file("node.key");

    store_ed25519_private_key(cluster.keys.at("a"), path);
    const auto loaded = load_ed25519_key_pair(path);
    assert(loaded.public_key == cluster.keys.at("a").public_key);
    assert(loaded.private_key_pem == cluster.keys.at("a").private_key_pem);

    const auto signature = sign(loaded.private_key_pem, "node-auth-message");
    assert(signature.size() == 64);
    assert(verify(loaded.public_key, "node-auth-message", signature));
    assert(!verify(loaded.public_key, "node-auth-messagf", signature));
    assert(!verify(cluster.keys.at("b").public_key, "node-auth-message", signature));
    auto tampered = signature;
    tampered[9] ^= 0x40;
    assert(!verify(loaded.public_key, "node-auth-message", tampered));
    assert(!verify(loaded.public_key, "node-auth-message", signature.substr(0, 63)));

    auto credentials = make_credentials(cluster.keys.at("a"));
    const auto wired = credentials.sign("wiring");
    assert(credentials.verify(cluster.keys.at("a").public_key, "wiring", wired));
    assert(!credentials.verify(cluster.keys.at("b").public_key, "wiring", wired));

    // Permission hygiene: anything group/other-visible is refused, a plain
    // owner-only file (even read-only) loads.
    chmod_file(path, 0644);
    assert(throws_runtime_error([&] { (void)load_ed25519_key_pair(path); }));
    chmod_file(path, 0666);
    assert(throws_runtime_error([&] { (void)load_ed25519_key_pair(path); }));
    chmod_file(path, 0600);
    (void)load_ed25519_key_pair(path);
    chmod_file(path, 0400);
    (void)load_ed25519_key_pair(path);

    assert(throws_runtime_error([&] { (void)load_ed25519_key_pair(dir.file("missing.key")); }));
    assert(throws_runtime_error([&] { (void)load_ed25519_key_pair(dir.path); })); // directory
    write_file(dir.file("garbage.key"), "definitely not a pem\n");
    chmod_file(dir.file("garbage.key"), 0600);
    assert(throws_runtime_error([&] { (void)load_ed25519_key_pair(dir.file("garbage.key")); }));

    // TrustStore epoch rules.
    auto store = cluster.store(1, {"b", "c"});
    assert(store->epoch() == 1 && store->trusts("b") && !store->trusts("a"));
    assert(store->neighbor_key("b") == cluster.keys.at("b").public_key);
    assert(store->neighbor_key("a").empty());
    TrustStore::Snapshot stale;
    stale.epoch = 1;
    stale.neighbors.push_back({"a", cluster.keys.at("a").public_key});
    assert(!store->apply(stale).has_value()); // stale epoch ignored
    assert(!store->trusts("a"));
    TrustStore::Snapshot next;
    next.epoch = 2;
    next.neighbors.push_back({"a", cluster.keys.at("a").public_key});
    std::atomic<int> fired{0};
    store->add_listener([&](const TrustStore::Snapshot& snapshot) {
        const bool knows_a = std::any_of(snapshot.neighbors.begin(), snapshot.neighbors.end(),
                                         [](const TrustStore::Neighbor& n) { return n.node_id == "a"; });
        if (snapshot.epoch == 2 && knows_a) ++fired;
    });
    assert(store->apply(next).has_value());
    assert(store->epoch() == 2 && store->trusts("a"));
    assert(fired.load() == 1);
    TrustStore::Snapshot bogus;
    bogus.epoch = 3;
    bogus.neighbors.push_back({"", cluster.keys.at("a").public_key}); // malformed entry
    assert(throws_runtime_error([&] { (void)store->apply(bogus); }));
    assert(throws_runtime_error([&] {
        TrustStore::Snapshot broken;
        broken.epoch = 4;
        TrustStore broken_store(std::string(31, 'k'), broken); // controller key must be 32 bytes
        (void)broken_store;
    }));
}

// ---- frame-type codec rules for the v1 control types ----

void control_frame_codec() {
    Cluster cluster;
    const auto update = signed_epoch_update(cluster, 2, {{"b", cluster.keys.at("b").public_key}}, {"c"});
    Frame frame{};
    assert(decode_frame(wire_frame(FrameType::epoch_config, update).substr(4), 1024 * 1024, frame));
    assert(frame.type == FrameType::epoch_config && frame.id == 0 && frame.payload == update);
    assert(decode_frame(wire_frame(FrameType::hello_auth, std::string(64, 'x')).substr(4), 1024 * 1024, frame));
    assert(frame.type == FrameType::hello_auth);
    assert(!decode_frame(wire_frame(FrameType::epoch_config, update).substr(4, 9), 1024 * 1024, frame));
    std::string nonzero_id = wire_frame(FrameType::epoch_config, update);
    nonzero_id[10] = 1; // request id must stay zero on control frames
    assert(!decode_frame(nonzero_id.substr(4), 1024 * 1024, frame));
    std::string unknown = wire_frame(FrameType::hello_auth, {});
    unknown[5] = 9; // type 9 does not exist yet
    assert(!decode_frame(unknown.substr(4), 1024 * 1024, frame));
}

void auth_roundtrip(PollerBackend backend) {
    Cluster cluster;
    NativeRuntime runtime(1, backend);
    runtime.start();
    ServerPool pool;
    auto listener = listen_on(runtime, pool, auth_options(cluster, "b", cluster.store(1, {"a"}), "a"));
    auto client = NodeLink::client(runtime.loop_for(0),
                                   auth_options(cluster, "a", cluster.store(1, {"b"}), "b"));
    client->start("127.0.0.1", listener.port);
    await([&] { return client->ready() && pool.any_ready(); });
    assert(client->peer_id() == "b" && client->peer_epoch() == 1);
    const auto server = pool.link(0);
    assert(server->peer_id() == "a" && server->peer_epoch() == 1);
    std::promise<std::string> echoed;
    client->request("ping", 1s, [&](std::error_code error, std::string payload) {
        assert(!error);
        echoed.set_value(std::move(payload));
    });
    assert(echoed.get_future().get() == "ping");
    std::promise<std::string> notified;
    server->on_notify([&](uint64_t, const std::string& payload) { notified.set_value(payload); });
    client->notify("one-way");
    assert(notified.get_future().wait_for(2s) == std::future_status::ready);
    client->stop();
    listener.stop();
    runtime.stop(true);
}

void forged_signature_rejected(PollerBackend backend) {
    Cluster cluster;
    NativeRuntime runtime(1, backend);
    runtime.start();
    ServerPool pool;
    // The store "trusts" a under the WRONG key: a genuine signature from a
    // must fail verification.
    TrustStore::Snapshot snapshot;
    snapshot.epoch = 1;
    snapshot.neighbors.push_back({"a", cluster.keys.at("eve").public_key});
    auto wrong_store = std::make_shared<TrustStore>(cluster.controller.public_key, std::move(snapshot));
    auto listener = listen_on(runtime, pool, auth_options(cluster, "b", wrong_store, "a"));
    auto client = NodeLink::client(runtime.loop_for(0),
                                   auth_options(cluster, "a", cluster.store(1, {"b"}), "b"));
    client->start("127.0.0.1", listener.port);
    await([&] { return client->connect_attempts() >= 2; });
    // The client may briefly see "ready" (it authenticated the server before
    // the server rejected the forged client signature); the server side must
    // never accept the link.
    assert(!pool.any_ready());
    assert(pool.error_count(std::errc::permission_denied) >= 1);
    client->stop();
    listener.stop();
    runtime.stop(true);
}

void outsider_rejected(PollerBackend backend) {
    Cluster cluster;
    NativeRuntime runtime(1, backend);
    runtime.start();
    ServerPool pool;
    // eve holds valid credentials but is nobody's neighbor.
    auto listener = listen_on(runtime, pool, auth_options(cluster, "b", cluster.store(1, {"a"}), "a"));
    auto eve = NodeLink::client(runtime.loop_for(0),
                                auth_options(cluster, "eve", cluster.store(1, {"b"}), "b"));
    eve->start("127.0.0.1", listener.port);
    await([&] { return eve->connect_attempts() >= 2; });
    assert(!eve->ready() && !pool.any_ready());
    assert(pool.error_count(std::errc::permission_denied) >= 1);
    // A plain v0 hello (node_id only) is not a valid challenge either.
    ServerPool pool2;
    auto listener2 = listen_on(runtime, pool2, auth_options(cluster, "b", cluster.store(1, {"a"}), "a"), 1);
    ScriptedPeer plain;
    plain.connect(runtime, listener2.port);
    plain.send(wire_frame(FrameType::hello, "a"));
    await([&] { return pool2.error_count(std::errc::protocol_error) >= 1; });
    plain.close();
    eve->stop();
    listener.stop();
    listener2.stop();
    runtime.stop(true);
}

// Replay against a server: the recorded challenge-auth pair cannot satisfy a
// fresh server nonce (signature binding), and a client refuses a replayed
// challenge nonce outright (per-link nonce cache surviving reconnects).
void replay_rejected(PollerBackend backend) {
    Cluster cluster;
    NativeRuntime runtime(1, backend);
    runtime.start();

    // Part 1 - server side. A scripted genuine client completes a real
    // handshake; the attacker then replays its recorded frames against a
    // fresh server link.
    ServerPool pool;
    auto listener = listen_on(runtime, pool, auth_options(cluster, "b", cluster.store(1, {"a"}), "a"));
    ScriptedPeer genuine;
    genuine.connect(runtime, listener.port);
    const std::string client_nonce(32, '\x11');
    const std::string genuine_challenge =
        wire_frame(FrameType::hello, wire_challenge("a", 1, client_nonce));
    genuine.send(genuine_challenge);
    const auto server_wire = genuine.read_wire();
    assert(server_wire.has_value());
    FrameType type = FrameType::hello;
    std::string payload;
    assert(parse_wire_frame(*server_wire, type, payload));
    std::string server_id, server_nonce;
    uint64_t server_epoch = 0;
    assert(type == FrameType::hello && parse_wire_challenge(payload, server_id, server_epoch, server_nonce));
    assert(server_id == "b" && server_epoch == 1);
    const auto signature =
        sign(cluster.keys.at("a").private_key_pem,
             wire_auth_message("a", client_nonce, server_id, server_nonce));
    const std::string genuine_auth =
        wire_frame(FrameType::hello_auth, wire_auth("a", 1, signature));
    genuine.send(genuine_auth);
    await([&] { return pool.any_ready(); });
    genuine.close();

    ScriptedPeer attacker;
    attacker.connect(runtime, listener.port);
    const auto bait = attacker.read_wire(); // server challenge for the attacker's own connection
    assert(bait.has_value());
    attacker.send(genuine_challenge); // replayed challenge: well-formed, trusted identity
    const auto server_auth = attacker.read_wire(); // fresh server nonce binding
    assert(server_auth.has_value());
    attacker.send(genuine_auth); // replayed signature: signed the PREVIOUS server nonce
    await([&] { return pool.error_count(std::errc::permission_denied) >= 1; });
    attacker.close();
    listener.stop();

    // Part 2 - client side. A scripted server replays its own previous
    // challenge bytes on the client's next connection; the nonce cache must
    // refuse them, and a fresh challenge must still work afterwards.
    auto client = NodeLink::client(runtime.loop_for(0), auth_options(cluster, "a", cluster.store(1, {"b"}), "b"));
    std::mutex client_errors_mutex;
    std::vector<std::error_code> client_errors;
    client->on_state([&](bool ready, std::error_code error) {
        if (!ready && error) {
            std::lock_guard<std::mutex> lock(client_errors_mutex);
            client_errors.push_back(error);
        }
    });
    ServerTransportConfig config;
    config.address = "127.0.0.1";
    config.listeners.emplace_back(ListenerType::TCP, port_hint());
    auto raw_listener = make_native_server_transport(config, {runtime.loop_for(0)});
    const auto raw_port = raw_listener->listeners().front().port;
    std::mutex raw_mutex;
    std::shared_ptr<IConnection> live;
    raw_listener->start([&](std::unique_ptr<IConnection> connection, const ListenerConfig&) {
        std::lock_guard<std::mutex> lock(raw_mutex);
        live = std::move(connection);
    });
    auto take_live = [&]() -> std::shared_ptr<IConnection> {
        await([&] {
            std::lock_guard<std::mutex> lock(raw_mutex);
            return static_cast<bool>(live);
        });
        std::lock_guard<std::mutex> lock(raw_mutex);
        return live;
    };
    auto drop_live = [&]() {
        std::shared_ptr<IConnection> raw;
        {
            std::lock_guard<std::mutex> lock(raw_mutex);
            raw = live;
            live.reset();
        }
        close_raw(raw);
    };
    client->start("127.0.0.1", raw_port);

    // Connection 1: genuine scripted handshake, client becomes ready.
    const std::string first_nonce(32, '\x22');
    auto connection = take_live();
    assert(scripted_server_handshake(*connection, cluster, "b", cluster.keys.at("b"), 1, first_nonce));
    await([&] { return client->ready(); });
    drop_live();

    // Connection 2: replay the same challenge bytes.
    await([&] { return !client->ready(); });
    connection = take_live();
    const auto client_wire = read_one_wire(*connection); // client re-sent its challenge
    assert(client_wire.has_value());
    assert(write_wire(*connection,
                      wire_frame(FrameType::hello, wire_challenge("b", 1, first_nonce))));
    await([&] {
        std::lock_guard<std::mutex> lock(client_errors_mutex);
        return std::any_of(client_errors.begin(), client_errors.end(),
                           [](std::error_code e) { return e == std::make_error_code(std::errc::permission_denied); });
    });
    assert(!client->ready());
    drop_live();

    // Connection 3: a fresh challenge completes again.
    const std::string second_nonce(32, '\x33');
    connection = take_live();
    assert(scripted_server_handshake(*connection, cluster, "b", cluster.keys.at("b"), 1, second_nonce));
    await([&] { return client->ready(); });
    client->stop();
    drop_live();
    raw_listener->stop();
    runtime.stop(true);
}

// Epoch config: stale epochs and bad signatures are ignored, a higher
// controller-signed epoch applies atomically to the shared store.
void epoch_config_rules(PollerBackend backend) {
    Cluster cluster;
    NativeRuntime runtime(1, backend);
    runtime.start();
    ServerPool pool;
    auto listener = listen_on(runtime, pool, auth_options(cluster, "b", cluster.store(1, {"a"}), "a"));
    auto a_store = cluster.store(1, {"b"});
    auto client = NodeLink::client(runtime.loop_for(0), auth_options(cluster, "a", a_store, "b"));
    std::mutex client_errors_mutex;
    std::vector<std::error_code> client_errors;
    client->on_state([&](bool ready, std::error_code error) {
        if (!ready && error) {
            std::lock_guard<std::mutex> lock(client_errors_mutex);
            client_errors.push_back(error);
        }
    });
    client->start("127.0.0.1", listener.port);
    await([&] { return client->ready() && pool.any_ready(); });
    const auto server = pool.link(0);

    // Stale (== current) epoch, correctly signed: ignored.
    server->send_epoch_config(signed_epoch_update(
        cluster, 1, {{"a", cluster.keys.at("a").public_key}}, {}));
    std::this_thread::sleep_for(40ms);
    assert(a_store->epoch() == 1 && client->ready());

    // Higher epoch with a NON-controller signature: ignored.
    server->send_epoch_config(signed_epoch_update(
        cluster, 3, {{"a", cluster.keys.at("a").public_key}}, {}, &cluster.keys.at("eve")));
    std::this_thread::sleep_for(40ms);
    assert(a_store->epoch() == 1 && client->ready());

    // Higher epoch, controller-signed: applied.
    server->send_epoch_config(signed_epoch_update(
        cluster, 2, {{"a", cluster.keys.at("a").public_key}, {"b", cluster.keys.at("b").public_key}}, {}));
    await([&] { return a_store->epoch() == 2; });
    assert(client->ready());

    // Malformed control payload: a protocol error, the link dies.
    server->send_epoch_config(std::string("\x01\x00", 2));
    await([&] { return !client->ready(); });
    {
        std::lock_guard<std::mutex> lock(client_errors_mutex);
        assert(std::any_of(client_errors.begin(), client_errors.end(),
                           [](std::error_code e) { return e == std::make_error_code(std::errc::protocol_error); }));
    }
    client->stop();
    listener.stop();
    runtime.stop(true);
}

// Revocation propagation: an epoch update that drops a neighbor cuts that
// link (and only that link) and the client never dials the revoked peer again.
void revocation_cuts_link(PollerBackend backend) {
    Cluster cluster;
    NativeRuntime runtime(1, backend);
    runtime.start();
    ServerPool b_pool, c_pool;
    auto b_listener = listen_on(runtime, b_pool, auth_options(cluster, "b", cluster.store(1, {"a"}), "a"));
    auto c_listener = listen_on(runtime, c_pool, auth_options(cluster, "c", cluster.store(1, {"a"}), "a"), 1);
    auto a_store = cluster.store(1, {"b", "c"});
    auto to_b = NodeLink::client(runtime.loop_for(0), auth_options(cluster, "a", a_store, "b"));
    auto to_c = NodeLink::client(runtime.loop_for(0), auth_options(cluster, "a", a_store, "c"));
    std::mutex errors_mutex;
    std::vector<std::error_code> to_c_errors;
    to_c->on_state([&](bool ready, std::error_code error) {
        if (!ready && error) {
            std::lock_guard<std::mutex> lock(errors_mutex);
            to_c_errors.push_back(error);
        }
    });
    to_b->start("127.0.0.1", b_listener.port);
    to_c->start("127.0.0.1", c_listener.port);
    await([&] { return to_b->ready() && to_c->ready(); });

    // Epoch 2 arrives over the surviving b link: c is gone from the table.
    b_pool.link(0)->send_epoch_config(signed_epoch_update(
        cluster, 2, {{"a", cluster.keys.at("a").public_key}, {"b", cluster.keys.at("b").public_key}}, {"c"}));
    await([&] { return a_store->epoch() == 2 && !to_c->ready(); });
    {
        std::lock_guard<std::mutex> lock(errors_mutex);
        assert(std::any_of(to_c_errors.begin(), to_c_errors.end(),
                           [](std::error_code e) { return e == std::make_error_code(std::errc::permission_denied); }));
    }
    // to_b keeps working; to_c never dials again.
    await([&] { return b_pool.any_ready(); });
    const auto frozen = to_c->connect_attempts();
    std::this_thread::sleep_for(200ms);
    assert(to_c->connect_attempts() == frozen);
    assert(to_b->ready());
    to_b->stop();
    to_c->stop();
    b_listener.stop();
    c_listener.stop();
    runtime.stop(true);
}

// Key rotation: the epoch carries the neighbor's new public key; existing
// links stay up, fresh handshakes verify against the new key, and the old
// key stops being accepted.
void key_rotation(PollerBackend backend) {
    Cluster cluster;
    NativeRuntime runtime(1, backend);
    runtime.start();
    const auto rotated = generate_ed25519_key_pair();
    // The b-side listener signs with whatever key `current` holds and trusts
    // through one shared store, so accepts model the neighbor having reloaded
    // its bundle.
    auto current = std::make_shared<Ed25519KeyPair>(cluster.keys.at("b"));
    auto b_store = cluster.store(1, {"a"});
    ServerPool pool;
    ServerTransportConfig config;
    config.address = "127.0.0.1";
    config.listeners.emplace_back(ListenerType::TCP, port_hint());
    auto listener = make_native_server_transport(config, {runtime.loop_for(0)});
    const auto port = listener->listeners().front().port;
    listener->start([&](std::unique_ptr<IConnection> connection, const ListenerConfig&) {
        LinkOptions options;
        options.node_id = "b";
        options.expected_peer_id = "a";
        options.heartbeat_interval = 35ms;
        options.heartbeat_timeout = 65ms;
        options.trust = b_store;
        options.credentials = make_credentials(*current);
        auto server = NodeLink::accepted(std::move(connection), std::move(options));
        server->on_request([weak = std::weak_ptr<NodeLink>(server)](uint64_t id, const std::string& payload) {
            if (auto link = weak.lock()) if (payload != "ignore") link->respond(id, payload);
        });
        std::lock_guard<std::mutex> lock(pool.mutex);
        pool.links.push_back(std::move(server));
    });
    auto a_store = cluster.store(1, {"b"});
    auto client = NodeLink::client(runtime.loop_for(0), auth_options(cluster, "a", a_store, "b"));
    client->start("127.0.0.1", port);
    await([&] { return client->ready(); });

    // Controller publishes epoch 2 carrying b's rotated key; the b side
    // reloads its bundle to match. Existing links stay up throughout.
    *current = rotated;
    pool.link(0)->send_epoch_config(signed_epoch_update(
        cluster, 2, {{"a", cluster.keys.at("a").public_key}, {"b", rotated.public_key}}, {}));
    await([&] { return a_store->epoch() == 2; });
    assert(client->ready());
    // b learns epoch 2 through its own (per-node view of the) update.
    client->send_epoch_config(signed_epoch_update(
        cluster, 2, {{"a", cluster.keys.at("a").public_key}, {"b", rotated.public_key}}, {}));
    await([&] { return b_store->epoch() == 2; });

    // A fresh handshake must now use the new key and see the new epoch.
    auto refreshed = NodeLink::client(runtime.loop_for(0), auth_options(cluster, "a", a_store, "b"));
    // (reuse the same listener: new accepts sign with the rotated key)
    refreshed->start("127.0.0.1", port);
    await([&] { return refreshed->ready(); });
    assert(refreshed->peer_epoch() == 2);
    refreshed->stop();

    // The old key no longer verifies: a link presenting it is refused.
    LinkOptions stale_options = auth_options(cluster, "b", cluster.store(1, {"a"}), "a");
    stale_options.credentials = make_credentials(cluster.keys.at("b")); // pre-rotation key
    ServerPool stale_pool;
    auto stale_listener = listen_on(runtime, stale_pool, stale_options, 1);
    auto observer = NodeLink::client(runtime.loop_for(0), auth_options(cluster, "a", a_store, "b"));
    std::mutex observer_errors_mutex;
    std::vector<std::error_code> observer_errors;
    observer->on_state([&](bool ready, std::error_code error) {
        if (!ready && error) {
            std::lock_guard<std::mutex> lock(observer_errors_mutex);
            observer_errors.push_back(error);
        }
    });
    observer->start("127.0.0.1", stale_listener.port);
    await([&] { return observer->connect_attempts() >= 2; });
    assert(!observer->ready());
    {
        std::lock_guard<std::mutex> lock(observer_errors_mutex);
        assert(std::any_of(observer_errors.begin(), observer_errors.end(),
                           [](std::error_code e) { return e == std::make_error_code(std::errc::permission_denied); }));
    }
    observer->stop();
    client->stop();
    listener->stop();
    stale_listener.stop();
    runtime.stop(true);
}

// Challenge window: a peer that opens a connection and starts (but never
// finishes) the handshake is dropped by the heartbeat timeout.
void handshake_challenge_timeout(PollerBackend backend) {
    Cluster cluster;
    NativeRuntime runtime(1, backend);
    runtime.start();
    ServerPool pool;
    auto listener = listen_on(runtime, pool, auth_options(cluster, "b", cluster.store(1, {"a"}), "a"));
    ScriptedPeer stalled;
    stalled.connect(runtime, listener.port);
    stalled.send(wire_frame(FrameType::hello, wire_challenge("a", 1, std::string(32, '\x44'))));
    await([&] { return pool.error_count(std::errc::timed_out) >= 1; });
    assert(!pool.any_ready());
    stalled.close();
    listener.stop();
    runtime.stop(true);
}

#ifdef NODE_AUTH_TEST_HAVE_NATIVE_TLS
// In-process self-signed RSA channel certificate (TLS transport only; node
// identity keys above are Ed25519 and independent of this certificate).
struct PemPair {
    std::string cert;
    std::string key;
};
PemPair make_self_signed(const char* common_name, const char* sans) {
    EVP_PKEY* pkey = EVP_RSA_gen(2048);
    assert(pkey != nullptr);
    X509* x509 = X509_new();
    assert(x509 != nullptr);
    assert(X509_set_version(x509, 2) == 1);
    assert(ASN1_INTEGER_set(X509_get_serialNumber(x509), static_cast<long>(::getpid())) == 1);
    assert(X509_gmtime_adj(X509_getm_notBefore(x509), -3600) != nullptr);
    assert(X509_gmtime_adj(X509_getm_notAfter(x509), 86400) != nullptr);
    assert(X509_set_pubkey(x509, pkey) == 1);
    X509_NAME* name = const_cast<X509_NAME*>(X509_get_subject_name(x509));
    assert(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                      reinterpret_cast<const unsigned char*>(common_name), -1, -1, 0) == 1);
    assert(X509_set_issuer_name(x509, name) == 1);
    X509_EXTENSION* extension = X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name, sans);
    assert(extension != nullptr);
    assert(X509_add_ext(x509, extension, -1) == 1);
    X509_EXTENSION_free(extension);
    assert(X509_sign(x509, pkey, EVP_sha256()) != 0);
    auto to_pem = [](auto write) {
        BIO* bio = BIO_new(BIO_s_mem());
        assert(bio != nullptr);
        assert(write(bio) == 1);
        char* data = nullptr;
        const long size = BIO_get_mem_data(bio, &data);
        std::string pem(data, static_cast<std::size_t>(size));
        BIO_free(bio);
        return pem;
    };
    PemPair pair;
    pair.cert = to_pem([&](BIO* bio) { return PEM_write_bio_X509(bio, x509); });
    pair.key = to_pem(
        [&](BIO* bio) { return PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr); });
    X509_free(x509);
    EVP_PKEY_free(pkey);
    return pair;
}

// The authenticated handshake over a TLS channel; TLS encrypts, the frame
// layer still proves identity.
void tls_link_with_auth(PollerBackend backend) {
    Cluster cluster;
    TmpDir dir;
    const auto pem = make_self_signed("localhost", "DNS:localhost,IP:127.0.0.1");
    write_file(dir.file("server.crt"), pem.cert);
    write_file(dir.file("server.key"), pem.key);
    NativeRuntime runtime(1, backend);
    runtime.start();
    TlsConfig tls;
    tls.certificate_file = dir.file("server.crt");
    tls.private_key_file = dir.file("server.key");
    ServerPool pool;
    auto listener = listen_on(runtime, pool, auth_options(cluster, "b", cluster.store(1, {"a"}), "a"),
                              0, /*ssl=*/true, &tls);
    auto client_options = auth_options(cluster, "a", cluster.store(1, {"b"}), "b");
    client_options.use_tls = true;
    client_options.tls.server_name = "localhost";
    client_options.tls.ca_pem = pem.cert;
    client_options.tls.use_system_ca = false;
    auto client = NodeLink::client(runtime.loop_for(0), client_options);
    client->start("127.0.0.1", listener.port);
    await([&] { return client->ready() && pool.any_ready(); });
    assert(client->peer_id() == "b" && client->peer_epoch() == 1);
    std::promise<std::string> echoed;
    client->request("secret", 1s, [&](std::error_code error, std::string payload) {
        assert(!error);
        echoed.set_value(std::move(payload));
    });
    assert(echoed.get_future().get() == "secret");
    client->stop();
    listener.stop();
    runtime.stop(true);
}

// A client configured to require TLS never falls back to plaintext.
void tls_downgrade_refused(PollerBackend backend) {
    Cluster cluster;
    NativeRuntime runtime(1, backend);
    runtime.start();
    ServerPool pool;
    auto listener = listen_on(runtime, pool, auth_options(cluster, "b", cluster.store(1, {"a"}), "a"));
    auto client_options = auth_options(cluster, "a", cluster.store(1, {"b"}), "b");
    client_options.use_tls = true;
    client_options.tls.server_name = "localhost";
    client_options.tls.ca_pem = "-----BEGIN CERTIFICATE-----\nnope\n-----END CERTIFICATE-----\n";
    client_options.tls.use_system_ca = false;
    auto client = NodeLink::client(runtime.loop_for(0), client_options);
    client->start("127.0.0.1", listener.port);
    await([&] { return client->connect_attempts() >= 2; });
    assert(!client->ready() && !pool.any_ready());
    client->stop();
    listener.stop();
    runtime.stop(true);
}

// And a plaintext client cannot ride an SSL listener either.
void plain_client_vs_ssl_listener(PollerBackend backend) {
    Cluster cluster;
    TmpDir dir;
    const auto pem = make_self_signed("localhost", "DNS:localhost,IP:127.0.0.1");
    write_file(dir.file("server.crt"), pem.cert);
    write_file(dir.file("server.key"), pem.key);
    NativeRuntime runtime(1, backend);
    runtime.start();
    TlsConfig tls;
    tls.certificate_file = dir.file("server.crt");
    tls.private_key_file = dir.file("server.key");
    ServerPool pool;
    auto listener = listen_on(runtime, pool, auth_options(cluster, "b", cluster.store(1, {"a"}), "a"),
                              0, /*ssl=*/true, &tls);
    auto client = NodeLink::client(runtime.loop_for(0),
                                   auth_options(cluster, "a", cluster.store(1, {"b"}), "b"));
    client->start("127.0.0.1", listener.port);
    await([&] { return client->connect_attempts() >= 2; });
    assert(!client->ready() && !pool.any_ready());
    client->stop();
    listener.stop();
    runtime.stop(true);
}
#endif

} // namespace

int main() {
    credential_hygiene();
    control_frame_codec();
#ifdef NODE_AUTH_TEST_HAVE_NATIVE_TLS
    enable_native_tls();
#endif
    for (auto backend : available_backends()) {
        std::fprintf(stderr, "[case] auth_roundtrip %d\n", int(backend));
        auth_roundtrip(backend);
        std::fprintf(stderr, "[case] forged_signature_rejected %d\n", int(backend));
        forged_signature_rejected(backend);
        std::fprintf(stderr, "[case] outsider_rejected %d\n", int(backend));
        outsider_rejected(backend);
        std::fprintf(stderr, "[case] replay_rejected %d\n", int(backend));
        replay_rejected(backend);
        std::fprintf(stderr, "[case] epoch_config_rules %d\n", int(backend));
        epoch_config_rules(backend);
        std::fprintf(stderr, "[case] revocation_cuts_link %d\n", int(backend));
        revocation_cuts_link(backend);
        std::fprintf(stderr, "[case] key_rotation %d\n", int(backend));
        key_rotation(backend);
        std::fprintf(stderr, "[case] handshake_challenge_timeout %d\n", int(backend));
        handshake_challenge_timeout(backend);
#ifdef NODE_AUTH_TEST_HAVE_NATIVE_TLS
        std::fprintf(stderr, "[case] tls_link_with_auth %d\n", int(backend));
        tls_link_with_auth(backend);
        std::fprintf(stderr, "[case] tls_downgrade_refused %d\n", int(backend));
        tls_downgrade_refused(backend);
        std::fprintf(stderr, "[case] plain_client_vs_ssl_listener %d\n", int(backend));
        plain_client_vs_ssl_listener(backend);
#endif
    }
}
