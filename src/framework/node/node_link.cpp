#include "framework/node/node_link.h"
#include <algorithm>
#include <limits>
#include <random>
#include <stdexcept>
#include <utility>

namespace pr::node {
namespace {
std::error_code err(std::errc e) { return std::make_error_code(e); }
void put32(std::string& s, uint32_t n) { for (int i = 3; i >= 0; --i) s.push_back(char(n >> (i * 8))); }
void put64(std::string& s, uint64_t n) { for (int i = 7; i >= 0; --i) s.push_back(char(n >> (i * 8))); }
uint32_t get32(const char* p) { uint32_t n = 0; for (int i = 0; i < 4; ++i) n = (n << 8) | uint8_t(p[i]); return n; }
uint64_t get64(const char* p) { uint64_t n = 0; for (int i = 0; i < 8; ++i) n = (n << 8) | uint8_t(p[i]); return n; }

constexpr size_t kNonceBytes = 32;   // hello challenge randomness
constexpr size_t kSignatureBytes = 64; // raw Ed25519
constexpr size_t kNonceCache = 256;  // replay guard depth, survives reconnects
const char kAuthSep[] = "protorelay/node-auth/v1";

// Bounds-safe big-endian reader for the v1 handshake/control payloads.
struct Reader {
    const std::string& s;
    size_t pos = 0;
    bool u8(uint8_t& v) {
        if (pos + 1 > s.size()) return false;
        v = uint8_t(s[pos++]); return true;
    }
    bool u32(uint32_t& v) {
        if (pos + 4 > s.size()) return false;
        v = get32(s.data() + pos); pos += 4; return true;
    }
    bool u64(uint64_t& v) {
        if (pos + 8 > s.size()) return false;
        v = get64(s.data() + pos); pos += 8; return true;
    }
    bool fixed(size_t n, std::string& out) {
        if (n > s.size() - pos) return false;
        out.assign(s, pos, n); pos += n; return true;
    }
    bool sized(std::string& out) {
        uint32_t n = 0;
        if (!u32(n) || n > s.size() - pos) return false;
        out.assign(s, pos, n); pos += n; return true;
    }
    bool drained() const { return pos == s.size(); }
};

// Handshake challenge payload: version | node_id | epoch | 32-byte nonce.
std::string encode_challenge(const std::string& node_id, uint64_t epoch, const std::string& nonce) {
    std::string out;
    out.push_back(1);
    put32(out, uint32_t(node_id.size()));
    out += node_id;
    put64(out, epoch);
    out += nonce;
    return out;
}
bool decode_challenge(const std::string& payload, std::string& node_id, uint64_t& epoch, std::string& nonce) {
    Reader r{payload};
    uint8_t version = 0;
    if (!r.u8(version) || version != 1 || !r.sized(node_id) || !r.u64(epoch) ||
        !r.fixed(kNonceBytes, nonce) || !r.drained())
        return false;
    return true;
}

// Signed response payload: version | node_id | epoch | 64-byte signature.
std::string encode_auth_response(const std::string& node_id, uint64_t epoch, const std::string& signature) {
    std::string out;
    out.push_back(1);
    put32(out, uint32_t(node_id.size()));
    out += node_id;
    put64(out, epoch);
    out += signature;
    return out;
}
bool decode_auth_response(const std::string& payload, std::string& node_id, uint64_t& epoch, std::string& signature) {
    Reader r{payload};
    uint8_t version = 0;
    if (!r.u8(version) || version != 1 || !r.sized(node_id) || !r.u64(epoch) ||
        !r.fixed(kSignatureBytes, signature) || !r.drained())
        return false;
    return true;
}

// Signature input binds both identities and both fresh nonces; the pair is
// ordered by node id so both sides compute identical bytes.
std::string auth_message(std::string a_id, std::string a_nonce, std::string b_id, std::string b_nonce) {
    if (b_id < a_id) { std::swap(a_id, b_id); std::swap(a_nonce, b_nonce); }
    std::string out(kAuthSep);
    put32(out, uint32_t(a_id.size())); out += a_id; out += a_nonce;
    put32(out, uint32_t(b_id.size())); out += b_id; out += b_nonce;
    return out;
}

// Epoch-config payload: version | epoch | neighbor list | revoked list |
// 64-byte controller signature over the preceding bytes (canonical encoding).
bool decode_epoch_update(const std::string& payload, uint64_t& epoch,
                         std::vector<TrustStore::Neighbor>& neighbors, std::vector<std::string>& revoked,
                         std::string& signature) {
    if (payload.size() < kSignatureBytes + 1) return false;
    Reader r{payload};
    uint8_t version = 0;
    uint32_t neighbor_count = 0, revoked_count = 0;
    if (!r.u8(version) || version != 1 || !r.u64(epoch) || !r.u32(neighbor_count)) return false;
    for (uint32_t i = 0; i < neighbor_count; ++i) {
        TrustStore::Neighbor neighbor;
        if (!r.sized(neighbor.node_id) || !r.fixed(32, neighbor.public_key)) return false;
        neighbors.push_back(std::move(neighbor));
    }
    if (!r.u32(revoked_count)) return false;
    for (uint32_t i = 0; i < revoked_count; ++i) {
        std::string id;
        if (!r.sized(id)) return false;
        revoked.push_back(std::move(id));
    }
    if (!r.fixed(kSignatureBytes, signature) || !r.drained()) return false;
    return true;
}
} // namespace

std::string encode_epoch_update(uint64_t epoch, const std::vector<TrustStore::Neighbor>& neighbors,
                                const std::vector<std::string>& revoked) {
    std::string out;
    out.push_back(1);
    put64(out, epoch);
    put32(out, uint32_t(neighbors.size()));
    for (const auto& neighbor : neighbors) {
        put32(out, uint32_t(neighbor.node_id.size()));
        out += neighbor.node_id;
        out += neighbor.public_key;
    }
    put32(out, uint32_t(revoked.size()));
    for (const auto& id : revoked) {
        put32(out, uint32_t(id.size()));
        out += id;
    }
    return out;
}

std::string encode_frame(const Frame& frame) {
    if (frame.payload.size() > std::numeric_limits<uint32_t>::max() - 10) throw std::length_error("node frame too long");
    std::string out;
    out.reserve(14 + frame.payload.size());
    put32(out, static_cast<uint32_t>(10 + frame.payload.size()));
    out.push_back(1);
    out.push_back(char(frame.type));
    put64(out, frame.id);
    out += frame.payload;
    return out;
}
bool decode_frame(const std::string& body, size_t max_frame_bytes, Frame& out) {
    if (body.size() < 10 || body.size() > max_frame_bytes || uint8_t(body[0]) != 1) return false;
    const auto type = uint8_t(body[1]);
    if (type < 1 || type > 8) return false;
    const auto id = get64(body.data() + 2);
    if ((type == 4 || type == 5) ? id == 0 : id != 0) return false;
    out = {static_cast<FrameType>(type), id, body.substr(10)};
    return true;
}

NodeLink::NodeLink(std::shared_ptr<net::IExecutor> executor, LinkOptions options,
                   bool client, TransportFactory factory)
    : executor_(std::move(executor)), options_(std::move(options)), client_(client),
      authenticated_(bool(options_.credentials.sign)), factory_(std::move(factory)) {
    if (!executor_ || options_.node_id.empty() || options_.max_frame_bytes < 10 ||
        options_.max_frame_bytes > std::numeric_limits<uint32_t>::max() ||
        options_.max_pending_requests == 0 || options_.max_queued_frames == 0 ||
        options_.heartbeat_interval.count() <= 0 || options_.heartbeat_timeout.count() <= 0 ||
        options_.reconnect_min.count() <= 0 || options_.reconnect_max < options_.reconnect_min ||
        options_.node_id.size() > options_.max_frame_bytes - 10 ||
        bool(options_.credentials.sign) != bool(options_.credentials.verify) ||
        authenticated_ != bool(options_.trust))
        throw std::invalid_argument("invalid node link options");
}
NodeLink::~NodeLink() = default;
std::string NodeLink::peer_id() const {
    std::lock_guard<std::mutex> lock(peer_mutex_);
    return peer_id_;
}
uint64_t NodeLink::peer_epoch() const {
    std::lock_guard<std::mutex> lock(peer_mutex_);
    return peer_epoch_;
}
std::shared_ptr<NodeLink> NodeLink::client(std::shared_ptr<net::IExecutor> executor,
                                           LinkOptions options, TransportFactory factory) {
    if (!factory) factory = [executor] { return net::native::make_native_client_transport(executor); };
    auto link = std::shared_ptr<NodeLink>(new NodeLink(std::move(executor), std::move(options), true, std::move(factory)));
    link->register_trust_listener();
    return link;
}
std::shared_ptr<NodeLink> NodeLink::accepted(std::unique_ptr<IConnection> connection, LinkOptions options) {
    if (!connection) throw std::invalid_argument("missing connection");
    auto executor = connection->executor();
    auto link = std::shared_ptr<NodeLink>(new NodeLink(executor, std::move(options), false, {}));
    auto shared = std::shared_ptr<IConnection>(std::move(connection));
    link->register_trust_listener();
    executor->post([link, shared] { link->attach(shared); });
    return link;
}
void NodeLink::register_trust_listener() {
    if (!options_.trust) return;
    auto weak = weak_from_this();
    options_.trust->add_listener([weak](const TrustStore::Snapshot& snapshot) {
        if (auto link = weak.lock()) link->recheck_trust(snapshot);
    });
}
void NodeLink::start(const std::string& host, uint16_t port) {
    if (!client_) throw std::logic_error("accepted link cannot connect");
    auto self = shared_from_this();
    executor_->post([self, host, port] {
        if (self->stopped_ || self->connection_ || self->port_) return;
        self->host_ = host; self->port_ = port; self->begin_connect();
    });
}
void NodeLink::begin_connect() {
    if (stopped_) return;
    ++attempts_;
    auto transport = factory_();
    if (!transport || transport->executor() != executor_) { schedule_reconnect(); return; }
    auto self = shared_from_this();
    auto generation = ++generation_;
    connection_ = transport;
    transport->async_connect(host_, port_, [self, transport, generation](const std::error_code& error) {
        if (generation != self->generation_ || self->stopped_) return;
        if (error) { self->disconnect(error); return; }
        if (self->options_.use_tls) {
            // Channel encryption only; identity is established at the frame
            // layer regardless. A plaintext-only peer fails the upgrade here
            // instead of silently downgrading.
            transport->async_start_tls(self->options_.tls, [self, transport, generation](const std::error_code& tls_error) {
                if (generation != self->generation_ || self->stopped_) return;
                if (tls_error) { self->disconnect(tls_error); return; }
                self->attach(transport);
            });
            return;
        }
        self->attach(transport);
    });
}
void NodeLink::attach(std::shared_ptr<IConnection> connection) {
    if (stopped_) { connection->close(); return; }
    connection_ = std::move(connection);
    ++generation_;
    incoming_.clear();
    {
        std::lock_guard<std::mutex> lock(peer_mutex_);
        peer_id_.clear();
        peer_epoch_ = 0;
    }
    auth_state_ = AuthState::challenge; awaiting_pong_ = false;
    writing_ = false; writes_.clear();
    last_activity_ = std::chrono::steady_clock::now();
    bool queued = false;
    if (authenticated_) {
        own_challenge_ = Challenge{options_.node_id, options_.trust->epoch(), random_nonce()};
        queued = enqueue({FrameType::hello, 0,
                          encode_challenge(own_challenge_.node_id, own_challenge_.epoch, own_challenge_.nonce)});
    } else {
        queued = enqueue({FrameType::hello, 0, options_.node_id});
    }
    if (!queued) { disconnect(err(std::errc::no_buffer_space)); return; }
    read_next(generation_);
    tick(generation_);
}
std::string NodeLink::random_nonce() {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    std::string nonce(kNonceBytes, '\0');
    for (auto& byte : nonce) byte = char(uint8_t(rng() >> 24)); // discard low bits: mt19937 quality
    return nonce;
}
void NodeLink::read_next(uint64_t generation) {
    if (!connection_ || generation != generation_) return;
    auto self = shared_from_this();
    auto conn = connection_;
    auto storage = std::make_shared<std::vector<char>>(4096);
    conn->async_read(net::buffer(*storage), [self, conn, storage, generation](const std::error_code& error, size_t n) {
        if (generation != self->generation_ || self->stopped_) return;
        if (error || n == 0) { self->disconnect(error ? error : net::make_error_code(net::IoErrc::end_of_stream)); return; }
        self->last_activity_ = std::chrono::steady_clock::now();
        self->consume(storage->data(), n);
        if (generation == self->generation_) self->read_next(generation);
    });
}
void NodeLink::consume(const char* data, size_t size) {
    incoming_.append(data, size);
    while (incoming_.size() >= 4) {
        const size_t length = get32(incoming_.data());
        if (length < 10 || length > options_.max_frame_bytes) { disconnect(err(std::errc::protocol_error)); return; }
        if (incoming_.size() < length + 4) return;
        Frame frame{};
        if (!decode_frame(incoming_.substr(4, length), options_.max_frame_bytes, frame)) {
            disconnect(err(std::errc::protocol_error)); return;
        }
        incoming_.erase(0, length + 4);
        handle_frame(frame);
        if (!connection_) return;
    }
}
void NodeLink::handle_frame(const Frame& frame) {
    if (authenticated_) {
        switch (frame.type) {
        case FrameType::hello: handle_hello_challenge(frame); return;
        case FrameType::hello_auth: handle_hello_auth(frame); return;
        case FrameType::epoch_config:
            if (auth_state_ != AuthState::done) { disconnect(err(std::errc::protocol_error)); return; }
            apply_epoch_config(frame.payload);
            return;
        default: break;
        }
        if (auth_state_ != AuthState::done) { disconnect(err(std::errc::permission_denied)); return; }
    } else {
        if (frame.type == FrameType::hello_auth || frame.type == FrameType::epoch_config) {
            disconnect(err(std::errc::protocol_error)); return;
        }
        if (auth_state_ != AuthState::done) {
            if (frame.type != FrameType::hello || frame.payload.empty() ||
                (!options_.expected_peer_id.empty() && frame.payload != options_.expected_peer_id)) {
                disconnect(err(std::errc::permission_denied)); return;
            }
            set_peer(frame.payload, 0);
            auth_state_ = AuthState::done; reconnect_count_ = 0;
            ready_ = true; emit_state(true, {}); return;
        }
        if (frame.type == FrameType::hello) { disconnect(err(std::errc::protocol_error)); return; }
    }
    switch (frame.type) {
    case FrameType::ping: if (!enqueue({FrameType::pong, 0, {}})) disconnect(err(std::errc::no_buffer_space)); break;
    case FrameType::pong: awaiting_pong_ = false; break;
    case FrameType::request:
        if (request_handler_) {
            auto handler = request_handler_;
            executor_->post([handler, frame] { handler(frame.id, frame.payload); });
        }
        break;
    case FrameType::response: complete(frame.id, {}, frame.payload); break;
    case FrameType::notify:
        if (notify_handler_) {
            auto handler = notify_handler_;
            executor_->post([handler, frame] { handler(0, frame.payload); });
        }
        break;
    default: break;
    }
}
bool NodeLink::peer_acceptable(const std::string& node_id) const {
    if (node_id.empty() || node_id == options_.node_id) return false;
    if (!options_.trust->trusts(node_id)) return false;
    return options_.expected_peer_id.empty() || options_.expected_peer_id == node_id;
}
bool NodeLink::seen_nonce(const std::string& nonce) const {
    return std::find(seen_nonces_.begin(), seen_nonces_.end(), nonce) != seen_nonces_.end();
}
void NodeLink::remember_nonce(std::string nonce) {
    seen_nonces_.push_back(std::move(nonce));
    while (seen_nonces_.size() > kNonceCache) seen_nonces_.pop_front();
}
void NodeLink::set_peer(const std::string& node_id, uint64_t epoch) {
    std::lock_guard<std::mutex> lock(peer_mutex_);
    peer_id_ = node_id;
    peer_epoch_ = epoch;
}
void NodeLink::handle_hello_challenge(const Frame& frame) {
    if (auth_state_ != AuthState::challenge) { disconnect(err(std::errc::protocol_error)); return; }
    Challenge challenge;
    if (!decode_challenge(frame.payload, challenge.node_id, challenge.epoch, challenge.nonce)) {
        disconnect(err(std::errc::protocol_error)); return;
    }
    if (!peer_acceptable(challenge.node_id) || seen_nonce(challenge.nonce)) {
        disconnect(err(std::errc::permission_denied)); return; // unknown identity or replayed randomness
    }
    remember_nonce(challenge.nonce);
    peer_challenge_ = challenge;
    auth_state_ = AuthState::response;
    const auto message = auth_message(options_.node_id, own_challenge_.nonce, challenge.node_id, challenge.nonce);
    std::string signature;
    try {
        signature = options_.credentials.sign(message);
    } catch (const std::exception&) {
        disconnect(err(std::errc::operation_not_permitted)); return;
    }
    if (signature.size() != kSignatureBytes ||
        !enqueue({FrameType::hello_auth, 0, encode_auth_response(options_.node_id, options_.trust->epoch(), signature)})) {
        disconnect(err(std::errc::no_buffer_space)); return;
    }
}
void NodeLink::handle_hello_auth(const Frame& frame) {
    if (auth_state_ != AuthState::response) { disconnect(err(std::errc::protocol_error)); return; }
    std::string node_id, signature;
    uint64_t epoch = 0;
    if (!decode_auth_response(frame.payload, node_id, epoch, signature)) {
        disconnect(err(std::errc::protocol_error)); return;
    }
    if (node_id != peer_challenge_.node_id) { disconnect(err(std::errc::permission_denied)); return; }
    const auto key = options_.trust->neighbor_key(node_id);
    const auto message = auth_message(node_id, peer_challenge_.nonce, options_.node_id, own_challenge_.nonce);
    if (!options_.credentials.verify(key, message, signature)) {
        disconnect(err(std::errc::permission_denied)); return; // forged or rotated-away key
    }
    set_peer(node_id, epoch);
    auth_state_ = AuthState::done; reconnect_count_ = 0;
    ready_ = true; emit_state(true, {});
}
void NodeLink::apply_epoch_config(const std::string& payload) {
    uint64_t epoch = 0;
    std::string signature;
    std::vector<TrustStore::Neighbor> neighbors;
    std::vector<std::string> revoked;
    if (!decode_epoch_update(payload, epoch, neighbors, revoked, signature)) {
        disconnect(err(std::errc::protocol_error)); return;
    }
    const auto& controller = options_.trust->controller_public_key();
    const std::string message = payload.substr(0, payload.size() - kSignatureBytes);
    if (!options_.credentials.verify(controller, message, signature)) return; // bad signature: ignore
    TrustStore::Snapshot snapshot;
    snapshot.epoch = epoch;
    snapshot.neighbors = std::move(neighbors);
    snapshot.revoked = std::move(revoked);
    // Stale epochs are dropped by the store; revocation reactions (including
    // this link's) run through the registered listeners.
    (void)options_.trust->apply(std::move(snapshot));
}
void NodeLink::recheck_trust(const TrustStore::Snapshot& snapshot) {
    auto self = shared_from_this();
    executor_->post([self, snapshot] {
        if (self->stopped_) return;
        std::string target;
        {
            std::lock_guard<std::mutex> lock(self->peer_mutex_);
            if (!self->peer_id_.empty()) target = self->peer_id_;
        }
        if (target.empty()) target = self->options_.expected_peer_id;
        if (target.empty() || target == self->options_.node_id) return;
        const bool trusted = std::any_of(snapshot.neighbors.begin(), snapshot.neighbors.end(),
            [&](const TrustStore::Neighbor& neighbor) { return neighbor.node_id == target; });
        if (!trusted) {
            self->peer_revoked_ = true;
            self->disconnect(err(std::errc::permission_denied));
        }
    });
}
bool NodeLink::enqueue(Frame frame) {
    if (frame.payload.size() > options_.max_frame_bytes - 10 ||
        writes_.size() >= options_.max_queued_frames || !connection_) return false;
    writes_.push_back(std::make_shared<std::string>(encode_frame(frame)));
    if (!writing_) write_next(generation_);
    return true;
}
void NodeLink::write_next(uint64_t generation) {
    if (writes_.empty() || !connection_ || generation != generation_) { writing_ = false; return; }
    writing_ = true;
    auto self = shared_from_this(); auto conn = connection_; auto bytes = writes_.front();
    conn->async_write(net::buffer(*bytes), [self, conn, bytes, generation](const std::error_code& error, size_t) {
        if (generation != self->generation_ || self->stopped_) return;
        if (error) { self->disconnect(error); return; }
        self->writes_.pop_front(); self->write_next(generation);
    });
}
void NodeLink::tick(uint64_t generation) {
    if (generation != generation_ || !connection_ || stopped_) return;
    const auto now = std::chrono::steady_clock::now();
    auto delay = options_.heartbeat_interval;
    if (auth_state_ != AuthState::done) {
        // Challenge window: an unfinished (or replay-stalled) handshake must
        // complete within one heartbeat timeout.
        if (now - last_activity_ >= options_.heartbeat_timeout) { disconnect(err(std::errc::timed_out)); return; }
        delay = options_.heartbeat_timeout;
    } else if (awaiting_pong_) {
        if (now - ping_sent_ >= options_.heartbeat_timeout) { disconnect(err(std::errc::timed_out)); return; }
        delay = options_.heartbeat_timeout;
    } else if (now - last_activity_ >= options_.heartbeat_interval) {
        if (!enqueue({FrameType::ping, 0, {}})) { disconnect(err(std::errc::no_buffer_space)); return; }
        awaiting_pong_ = true; ping_sent_ = now; delay = options_.heartbeat_timeout;
    }
    heartbeat_timer_ = executor_->make_timer();
    heartbeat_timer_->expires_after(delay);
    auto weak = weak_from_this();
    heartbeat_timer_->async_wait([weak, generation](const std::error_code& error) {
        if (!error) if (auto self = weak.lock()) self->tick(generation);
    });
}
void NodeLink::complete(uint64_t id, std::error_code error, std::string payload) {
    auto it = pending_.find(id);
    if (it == pending_.end()) return;
    auto pending = std::move(it->second); pending_.erase(it);
    --outstanding_;
    pending.timer->cancel();
    executor_->post([completion = std::move(pending.completion), error, payload = std::move(payload)]() mutable {
        completion(error, std::move(payload));
    });
}
void NodeLink::emit_state(bool state, std::error_code error) {
    if (state_handler_) {
        auto cb = state_handler_;
        executor_->post([cb, state, error] { cb(state, error); });
    }
}
void NodeLink::disconnect(std::error_code error) {
    if (!connection_) return;
    const bool was_ready = ready_.exchange(false);
    {
        std::lock_guard<std::mutex> lock(peer_mutex_);
        peer_id_.clear();
        peer_epoch_ = 0;
    }
    ++generation_;
    auth_state_ = AuthState::challenge;
    if (heartbeat_timer_) { heartbeat_timer_->cancel(); heartbeat_timer_.reset(); }
    connection_->close(); connection_.reset();
    writes_.clear(); writing_ = false; incoming_.clear(); awaiting_pong_ = false;
    while (!pending_.empty()) complete(pending_.begin()->first, error, {});
    if (was_ready || error) emit_state(false, error);
    if (client_ && !stopped_ && !peer_revoked_) schedule_reconnect();
}
void NodeLink::schedule_reconnect() {
    if (stopped_) return;
    const auto exponent = std::min<uint64_t>(reconnect_count_++, 20);
    const auto cap = options_.reconnect_max.count();
    const auto base = std::min<int64_t>(cap, options_.reconnect_min.count() * (int64_t(1) << exponent));
    static thread_local std::mt19937_64 rng(std::random_device{}());
    const auto jitter = std::uniform_int_distribution<int64_t>(0, std::max<int64_t>(1, base / 4))(rng);
    reconnect_timer_ = executor_->make_timer();
    reconnect_timer_->expires_after(std::chrono::milliseconds(std::min<int64_t>(cap, base + jitter)));
    auto weak = weak_from_this();
    reconnect_timer_->async_wait([weak](const std::error_code& error) {
        if (!error) if (auto self = weak.lock()) self->begin_connect();
    });
}
uint64_t NodeLink::request(std::string payload, std::chrono::milliseconds timeout, Completion completion) {
    const auto id = next_id_.fetch_add(1);
    if (!completion) throw std::invalid_argument("request needs completion");
    if (outstanding_.fetch_add(1) >= options_.max_pending_requests) {
        --outstanding_;
        executor_->post([completion = std::move(completion)] { completion(err(std::errc::no_buffer_space), {}); });
        return id;
    }
    auto self = shared_from_this();
    executor_->post([self, id, payload = std::move(payload), timeout, completion = std::move(completion)]() mutable {
        auto fail = [&](std::errc error) {
            --self->outstanding_;
            self->executor_->post([cb = std::move(completion), error] { cb(err(error), {}); });
        };
        if (self->stopped_) { fail(std::errc::operation_canceled); return; }
        if (!self->ready_) { fail(std::errc::not_connected); return; }
        if (self->pending_.size() >= self->options_.max_pending_requests ||
            self->writes_.size() >= self->options_.max_queued_frames) { fail(std::errc::no_buffer_space); return; }
        if (payload.size() > self->options_.max_frame_bytes - 10) { fail(std::errc::message_size); return; }
        auto timer = self->executor_->make_timer();
        timer->expires_after(timeout);
        self->pending_.emplace(id, Pending{std::move(completion), timer});
        auto weak = self->weak_from_this();
        timer->async_wait([weak, id](const std::error_code& error) {
            if (!error) if (auto link = weak.lock()) link->complete(id, err(std::errc::timed_out), {});
        });
        if (!self->enqueue({FrameType::request, id, std::move(payload)}))
            self->complete(id, err(std::errc::no_buffer_space), {});
    });
    return id;
}
void NodeLink::cancel(uint64_t id) {
    auto self = shared_from_this(); executor_->post([self, id] { self->complete(id, err(std::errc::operation_canceled), {}); });
}
void NodeLink::respond(uint64_t id, std::string payload) {
    auto self = shared_from_this(); executor_->post([self, id, payload = std::move(payload)]() mutable {
        if (self->ready_ && !self->enqueue({FrameType::response, id, std::move(payload)}))
            self->disconnect(err(std::errc::no_buffer_space));
    });
}
void NodeLink::notify(std::string payload, std::function<void(std::error_code)> completion) {
    auto self = shared_from_this(); executor_->post([self, payload = std::move(payload), completion = std::move(completion)]() mutable {
        const auto error = self->stopped_ ? err(std::errc::operation_canceled) :
            !self->ready_ ? err(std::errc::not_connected) :
            !self->enqueue({FrameType::notify, 0, std::move(payload)}) ? err(std::errc::no_buffer_space) : std::error_code{};
        if (completion) self->executor_->post([completion = std::move(completion), error] { completion(error); });
    });
}
void NodeLink::send_epoch_config(std::string payload, std::function<void(std::error_code)> completion) {
    auto self = shared_from_this();
    executor_->post([self, payload = std::move(payload), completion = std::move(completion)]() mutable {
        const auto error = self->stopped_ ? err(std::errc::operation_canceled) :
            !self->ready_ ? err(std::errc::not_connected) :
            !self->enqueue({FrameType::epoch_config, 0, std::move(payload)}) ? err(std::errc::no_buffer_space) : std::error_code{};
        if (completion) self->executor_->post([completion = std::move(completion), error] { completion(error); });
    });
}
void NodeLink::on_request(MessageHandler handler) {
    auto self = shared_from_this(); executor_->post([self, handler = std::move(handler)]() mutable { self->request_handler_ = std::move(handler); });
}
void NodeLink::on_notify(MessageHandler handler) {
    auto self = shared_from_this(); executor_->post([self, handler = std::move(handler)]() mutable { self->notify_handler_ = std::move(handler); });
}
void NodeLink::on_state(StateHandler handler) {
    auto self = shared_from_this(); executor_->post([self, handler = std::move(handler)]() mutable { self->state_handler_ = std::move(handler); });
}
void NodeLink::stop() {
    stopped_ = true;
    auto self = shared_from_this(); executor_->post([self] { self->stop_on_executor(); });
}
void NodeLink::stop_on_executor() {
    if (reconnect_timer_) { reconnect_timer_->cancel(); reconnect_timer_.reset(); }
    disconnect(err(std::errc::operation_canceled));
    while (!pending_.empty()) complete(pending_.begin()->first, err(std::errc::operation_canceled), {});
}
} // namespace pr::node
