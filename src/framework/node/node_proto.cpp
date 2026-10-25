#include "framework/node/node_proto.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <random>
#include <set>
#include <stdexcept>
#include <utility>

#if defined(PR_NODE_PROTO_HAVE_OPENSSL_AEAD) && PR_NODE_PROTO_HAVE_OPENSSL_AEAD
#define PR_NODE_PROTO_AEAD 1
#include <openssl/evp.h>
#endif

namespace pr::node {
namespace {

constexpr size_t kFixedHeader = 28; // version through message_id
constexpr size_t kTailHeader = 4;   // payload length prefix
constexpr size_t kTagBytes = 16;    // AES-256-GCM tag

std::error_code err(std::errc e) { return std::make_error_code(e); }
void put16(std::string& s, uint16_t n) { s.push_back(char(n >> 8)); s.push_back(char(n)); }
void put32(std::string& s, uint32_t n) { for (int i = 3; i >= 0; --i) s.push_back(char(n >> (i * 8))); }
void put64(std::string& s, uint64_t n) { for (int i = 7; i >= 0; --i) s.push_back(char(n >> (i * 8))); }
uint16_t get16(const char* p) { return uint16_t((uint8_t(p[0]) << 8) | uint8_t(p[1])); }
uint32_t get32(const char* p) { uint32_t n = 0; for (int i = 0; i < 4; ++i) n = (n << 8) | uint8_t(p[i]); return n; }
uint64_t get64(const char* p) { uint64_t n = 0; for (int i = 0; i < 8; ++i) n = (n << 8) | uint8_t(p[i]); return n; }
uint64_t now_ms() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count());
}
uint32_t rand32() { static thread_local std::mt19937_64 rng(std::random_device{}()); return uint32_t(rng()); }
uint64_t rand64() { static thread_local std::mt19937_64 rng(std::random_device{}()); return rng(); }

std::string aead_nonce(uint64_t message_id, uint32_t nonce) {
    std::string out;
    put64(out, message_id);
    put32(out, nonce);
    return out; // 12 bytes, unique per message
}

// Header bytes only (everything before the payload). The AAD variant zeroes
// ttl and route_pos because every forwarding hop rewrites those two fields;
// sender and destination therefore derive identical AAD from the same
// message.
std::string encode_header(const Envelope& e, size_t payload_len) {
    std::string out;
    out.reserve(kFixedHeader + 4 + e.source.size() + e.destination.size() + 2 + 3 * e.route.size());
    out.push_back(char(e.version));
    out.push_back(char(e.type));
    out.push_back(e.encrypted ? '\1' : '\0');
    out.push_back(char(e.key_id));
    put32(out, e.ttl);
    put32(out, e.nonce);
    put64(out, e.timestamp_ms);
    put64(out, e.message_id);
    put16(out, uint16_t(e.source.size()));
    out += e.source;
    put16(out, uint16_t(e.destination.size()));
    out += e.destination;
    out.push_back(char(e.route.size()));
    out.push_back(char(e.route_pos));
    for (const auto& hop : e.route) {
        put16(out, uint16_t(hop.size()));
        out += hop;
    }
    put32(out, uint32_t(payload_len));
    return out;
}

#ifdef PR_NODE_PROTO_AEAD
// AES-256-GCM over `in`; the ciphertext output carries the 16-byte tag
// appended. Decrypt expects the tag as the last kTagBytes of `in`.
bool aead_crypt(bool encrypt, const std::string& key, const std::string& nonce,
                const std::string& aad, const std::string& in, std::string& out) {
    if (key.size() != 32 || nonce.size() != 12 || (!encrypt && in.size() < kTagBytes)) return false;
    bool ok = false;
    auto* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    do {
        const int begun = encrypt ? EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr)
                                  : EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
        if (begun != 1) break;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1) break;
        const auto* key_bytes = reinterpret_cast<const unsigned char*>(key.data());
        const auto* nonce_bytes = reinterpret_cast<const unsigned char*>(nonce.data());
        const int keyed = encrypt ? EVP_EncryptInit_ex(ctx, nullptr, nullptr, key_bytes, nonce_bytes)
                                  : EVP_DecryptInit_ex(ctx, nullptr, nullptr, key_bytes, nonce_bytes);
        if (keyed != 1) break;
        int len = 0;
        if (!aad.empty()) {
            const auto* aad_bytes = reinterpret_cast<const unsigned char*>(aad.data());
            const int aad_ok = encrypt ? EVP_EncryptUpdate(ctx, nullptr, &len, aad_bytes, int(aad.size()))
                                       : EVP_DecryptUpdate(ctx, nullptr, &len, aad_bytes, int(aad.size()));
            if (aad_ok != 1) break;
        }
        if (encrypt) {
            out.assign(in.size() + kTagBytes, '\0');
            if (EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char*>(out.data()), &len,
                                  reinterpret_cast<const unsigned char*>(in.data()), int(in.size())) != 1)
                break;
            size_t total = size_t(len);
            if (EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(out.data()) + total, &len) != 1) break;
            total += size_t(len);
            if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, int(kTagBytes), out.data() + total) != 1) break;
            out.resize(total + kTagBytes);
            ok = true;
        } else {
            const size_t body = in.size() - kTagBytes;
            out.assign(body, '\0');
            unsigned char tag[kTagBytes];
            std::memcpy(tag, in.data() + body, kTagBytes);
            if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, int(kTagBytes), tag) != 1) break;
            if (body != 0 &&
                EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(out.data()), &len,
                                  reinterpret_cast<const unsigned char*>(in.data()), int(body)) != 1)
                break;
            if (EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(out.data()), &len) != 1) break;
            ok = true;
        }
    } while (false);
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) out.clear();
    return ok;
}
#endif
} // namespace

std::string encode_envelope(const Envelope& e) {
    if (e.version != 1) throw std::invalid_argument("envelope version must be 1");
    if (e.source.empty() || e.source.size() > 255 || e.destination.empty() || e.destination.size() > 255)
        throw std::invalid_argument("envelope endpoints must be 1..255 bytes");
    if (e.route.size() > 255 || e.route_pos > e.route.size())
        throw std::invalid_argument("envelope route invalid");
    for (const auto& hop : e.route)
        if (hop.empty() || hop.size() > 255) throw std::invalid_argument("envelope route hop must be 1..255 bytes");
    if (e.payload.size() > std::numeric_limits<uint32_t>::max()) throw std::length_error("envelope payload too long");
    std::string wire = encode_header(e, e.payload.size());
    wire += e.payload;
    return wire;
}

DecodeResult decode_envelope(const std::string& wire, const NodeRouterOptions& limits, Envelope& out) {
    out = Envelope{};
    if (wire.size() < kFixedHeader + kTailHeader || wire.size() > limits.max_envelope_bytes)
        return DecodeResult::malformed;
    if (uint8_t(wire[0]) != 1) return DecodeResult::unknown_version;
    const uint8_t flags = uint8_t(wire[2]);
    if (flags & ~uint8_t(1)) return DecodeResult::malformed; // reserved bits must be zero
    const uint8_t type = uint8_t(wire[1]);
    out.version = 1;
    out.encrypted = (flags & 1) != 0;
    out.key_id = uint8_t(wire[3]);
    out.ttl = get32(wire.data() + 4);
    out.nonce = get32(wire.data() + 8);
    out.timestamp_ms = get64(wire.data() + 12);
    out.message_id = get64(wire.data() + 20);
    size_t pos = kFixedHeader;
    const auto take_id = [&](std::string& id) {
        if (pos + 2 > wire.size()) return false;
        const size_t n = get16(wire.data() + pos);
        pos += 2;
        if (n == 0 || n > limits.max_node_id_bytes || pos + n > wire.size()) return false;
        id.assign(wire, pos, n);
        pos += n;
        return true;
    };
    if (!take_id(out.source) || !take_id(out.destination)) return DecodeResult::malformed;
    if (pos + 2 > wire.size()) return DecodeResult::malformed;
    const size_t route_len = uint8_t(wire[pos++]);
    out.route_pos = uint8_t(wire[pos++]);
    if (route_len > limits.max_route_hops || out.route_pos > route_len) return DecodeResult::malformed;
    for (size_t i = 0; i < route_len; ++i) {
        std::string hop;
        if (!take_id(hop)) return DecodeResult::malformed;
        out.route.push_back(std::move(hop));
    }
    if (pos + kTailHeader > wire.size()) return DecodeResult::malformed;
    const size_t payload_len = get32(wire.data() + pos);
    pos += kTailHeader;
    if (payload_len != wire.size() - pos) return DecodeResult::malformed; // payload is last and exact
    out.payload = wire.substr(pos);
    if (type < 1 || type > 4) return DecodeResult::ok_unknown_type; // out carries the parsed routing fields
    out.type = MessageType(type);
    return DecodeResult::ok;
}

NodeRouter::NodeRouter(std::shared_ptr<net::IExecutor> executor, std::string node_id, NodeRouterOptions options)
    : executor_(std::move(executor)), node_id_(std::move(node_id)), options_(options) {
    window_ = std::chrono::seconds(std::max<int64_t>(1, (2 * options_.timestamp_tolerance.count() + 999) / 1000));
    nonces_ = std::make_unique<LruCache<std::string, std::shared_ptr<LruCache<uint32_t, char>>>>(
        options_.max_tracked_sources, window_);
    seen_ids_ = std::make_unique<LruCache<uint64_t, char>>(options_.seen_message_capacity, window_);
}
NodeRouter::~NodeRouter() = default;

std::shared_ptr<NodeRouter> NodeRouter::create(std::shared_ptr<net::IExecutor> executor, std::string node_id,
                                               NodeRouterOptions options) {
    if (!executor || node_id.empty() || node_id.size() > 255 || options.max_node_id_bytes == 0 ||
        options.max_node_id_bytes > 255 || options.max_route_hops == 0 || options.max_route_hops > 255 ||
        options.max_hops == 0 || options.timestamp_tolerance.count() < 0 || options.max_in_flight == 0 ||
        options.seen_message_capacity == 0 || options.nonce_capacity == 0 || options.max_tracked_sources == 0 ||
        options.max_envelope_bytes < kFixedHeader + kTailHeader)
        throw std::invalid_argument("invalid node router options");
    return std::shared_ptr<NodeRouter>(new NodeRouter(std::move(executor), std::move(node_id), options));
}

void NodeRouter::add_key(uint8_t key_id, const std::string& key) {
    if (key.size() != 32) throw std::invalid_argument("node proto key must be 32 bytes");
    auto self = shared_from_this();
    executor_->post([self, key_id, key] { self->keys_[key_id] = key; });
}

void NodeRouter::attach(const std::string& neighbor_id, std::shared_ptr<NodeLink> link) {
    if (neighbor_id.empty() || neighbor_id.size() > options_.max_node_id_bytes || !link)
        throw std::invalid_argument("invalid neighbor attach");
    auto self = shared_from_this();
    executor_->post([self, neighbor_id, link] {
        if (self->stopped_) return;
        self->links_[neighbor_id] = link;
        link->on_notify([weak = std::weak_ptr<NodeRouter>(self)](uint64_t, const std::string& body) {
            if (auto router = weak.lock()) router->receive(body);
        });
    });
}

void NodeRouter::detach(const std::string& neighbor_id) {
    auto self = shared_from_this();
    executor_->post([self, neighbor_id] { self->links_.erase(neighbor_id); });
}

void NodeRouter::on_message(ReceiveHandler handler) {
    auto self = shared_from_this();
    executor_->post([self, handler = std::move(handler)]() mutable { self->message_handler_ = std::move(handler); });
}

void NodeRouter::on_error_report(ErrorReportHandler handler) {
    auto self = shared_from_this();
    executor_->post([self, handler = std::move(handler)]() mutable { self->error_handler_ = std::move(handler); });
}

void NodeRouter::send(Envelope message, SendCompletion completion) {
    auto self = shared_from_this();
    executor_->post([self, message = std::move(message), completion = std::move(completion)]() mutable {
        self->run_send(std::move(message), std::move(completion));
    });
}

void NodeRouter::send_fresh(Envelope& m) {
    m.source = node_id_;
    if (!m.ttl) m.ttl = options_.max_hops;
    if (!m.nonce) m.nonce = rand32();
    if (!m.timestamp_ms) m.timestamp_ms = now_ms();
    if (!m.message_id) m.message_id = rand64();
    m.route_pos = 0;
}

void NodeRouter::run_send(Envelope m, SendCompletion completion) {
    const auto fail = [&](std::errc e) {
        if (completion) completion(err(e));
    };
    if (stopped_) return fail(std::errc::operation_canceled);
    if (m.destination.empty() || m.destination.size() > options_.max_node_id_bytes)
        return fail(std::errc::invalid_argument);
    if (m.payload.size() > options_.max_envelope_bytes) return fail(std::errc::message_size);
    if (m.route.size() > options_.max_route_hops) return fail(std::errc::invalid_argument);
    std::set<std::string> unique;
    for (const auto& hop : m.route)
        if (hop.empty() || hop.size() > options_.max_node_id_bytes || hop == node_id_ ||
            hop == m.destination || !unique.insert(hop).second)
            return fail(std::errc::invalid_argument);
    send_fresh(m);
    std::string wire;
    if (m.encrypted) {
#ifdef PR_NODE_PROTO_AEAD
        const auto key = keys_.find(m.key_id);
        if (key == keys_.end()) return fail(std::errc::not_supported);
        std::string plain = std::move(m.payload); // Envelope keeps plaintext API semantics
        Envelope aad_env = m;
        aad_env.ttl = 0;
        aad_env.route_pos = 0;
        std::string cipher;
        if (!aead_crypt(true, key->second, aead_nonce(m.message_id, m.nonce),
                        encode_header(aad_env, plain.size() + kTagBytes), plain, cipher))
            return fail(std::errc::protocol_error);
        wire = encode_header(m, cipher.size()) + cipher;
#else
        return fail(std::errc::not_supported);
#endif
    } else {
        wire = encode_envelope(m);
    }
    if (wire.size() > options_.max_envelope_bytes) return fail(std::errc::message_size);
    const std::string next = m.route.empty() ? m.destination : m.route.front();
    handoff(next, std::move(wire), ReturnPath{m.source, {}, m.message_id, true}, false, std::move(completion));
}

void NodeRouter::receive(const std::string& wire) {
    if (stopped_) return;
    Envelope e;
    switch (decode_envelope(wire, options_, e)) {
    case DecodeResult::malformed:
    case DecodeResult::unknown_version:
        return; // nothing trustworthy to report against; the link stays up
    case DecodeResult::ok_unknown_type:
        report(ReturnPath{e.source, return_route(e.route, e.route_pos), e.message_id, false},
               MessageErrorCode::unknown_type);
        return;
    case DecodeResult::ok:
        break;
    }
    if (e.destination == node_id_) return deliver(std::move(e));
    forward(std::move(e));
}

void NodeRouter::deliver(Envelope e) {
    const size_t consumed = std::min<size_t>(e.route_pos, e.route.size());
    const ReturnPath back{e.source, return_route(e.route, consumed), e.message_id, e.type == MessageType::error};
    const int64_t delta = int64_t(now_ms()) - int64_t(e.timestamp_ms);
    const int64_t tolerance = int64_t(options_.timestamp_tolerance.count());
    if (delta > tolerance || delta < -tolerance) {
        report(back, MessageErrorCode::replay_rejected);
        return;
    }
    if (nonce_seen_and_insert(e.source, e.nonce)) {
        report(back, MessageErrorCode::replay_rejected);
        return;
    }
    if (options_.dedup_by_message_id && message_id_seen_and_insert(e.message_id)) return;
    if (e.encrypted) {
        std::string plain;
        bool ok = false;
#ifdef PR_NODE_PROTO_AEAD
        const auto key = keys_.find(e.key_id);
        if (key != keys_.end()) {
            Envelope aad_env = e;
            aad_env.ttl = 0;
            aad_env.route_pos = 0;
            ok = aead_crypt(false, key->second, aead_nonce(e.message_id, e.nonce),
                            encode_header(aad_env, e.payload.size()), e.payload, plain);
        }
#endif
        if (!ok) {
            report(back, MessageErrorCode::decrypt_failed);
            return;
        }
        e.payload = std::move(plain);
    }
    if (e.type == MessageType::error) {
        if (e.payload.size() >= 9 && error_handler_) error_handler_(e, MessageErrorCode(uint8_t(e.payload[0])),
                                                                    get64(e.payload.data() + 1));
        return;
    }
    if (e.type == MessageType::probe) {
        Envelope ack;
        ack.type = MessageType::ack;
        ack.destination = e.source;
        ack.route = back.route;
        put64(ack.payload, e.message_id);
        ack.payload += e.payload; // [acked id][echoed probe payload]
        send_fresh(ack);
        handoff(ack.route.empty() ? ack.destination : ack.route.front(), encode_envelope(ack),
                ReturnPath{ack.source, {}, ack.message_id, true}, true, {});
        return;
    }
    if (message_handler_) message_handler_(e);
}

void NodeRouter::forward(Envelope e) {
    const size_t size = e.route.size();
    const size_t consumed = std::min<size_t>(e.route_pos, size);
    const ReturnPath back{e.source, return_route(e.route, consumed), e.message_id, e.type == MessageType::error};
    if (message_id_seen_and_insert(e.message_id)) return; // ring protection: silent drop
    if (!options_.forward_enabled) {
        report(back, MessageErrorCode::unroutable);
        return;
    }
    std::string next;
    if (e.route_pos < size) {
        if (e.route[e.route_pos] != node_id_) {
            report(back, MessageErrorCode::unroutable); // off-path arrival
            return;
        }
        next = e.route_pos + 1 < size ? e.route[e.route_pos + 1] : e.destination;
    } else {
        next = e.destination;
    }
    if (e.ttl == 0) {
        report(back, MessageErrorCode::ttl_exhausted);
        return;
    }
    --e.ttl;
    if (consumed < size) e.route_pos = uint8_t(consumed + 1);
    handoff(next, encode_envelope(e), back, false, {});
}

void NodeRouter::report(const ReturnPath& back, MessageErrorCode code) {
    if (back.silent || back.source.empty() || back.source == node_id_ || stopped_) return;
    Envelope r;
    r.type = MessageType::error;
    r.destination = back.source;
    r.route = back.route;
    r.payload.reserve(9);
    r.payload.push_back(char(code));
    put64(r.payload, back.message_id);
    send_fresh(r);
    handoff(r.route.empty() ? r.destination : r.route.front(), encode_envelope(r),
            ReturnPath{r.source, {}, r.message_id, true}, true, {});
}

void NodeRouter::handoff(const std::string& next, std::string wire, ReturnPath back, bool is_report,
                         SendCompletion completion) {
    auto self = shared_from_this();
    executor_->post([self, next, wire = std::move(wire), back, is_report,
                     completion = std::move(completion)]() mutable {
        if (self->stopped_) {
            if (completion) completion(err(std::errc::operation_canceled));
            return;
        }
        if (!is_report && self->in_flight_.fetch_add(1) >= self->options_.max_in_flight) {
            self->in_flight_.fetch_sub(1);
            self->report(back, MessageErrorCode::queue_overflow);
            if (completion) completion(err(std::errc::no_buffer_space));
            return;
        }
        const auto it = self->links_.find(next);
        if (it == self->links_.end()) {
            if (!is_report) self->in_flight_.fetch_sub(1);
            self->report(back, MessageErrorCode::unroutable);
            if (completion) completion(err(std::errc::host_unreachable));
            return;
        }
        auto link = it->second;
        auto weak = std::weak_ptr<NodeRouter>(self);
        link->notify(std::move(wire), [weak, back, is_report, completion = std::move(completion)](std::error_code ec) mutable {
            if (auto router = weak.lock()) {
                if (!is_report) --router->in_flight_;
                if (ec) router->report(back, ec == err(std::errc::no_buffer_space) ? MessageErrorCode::queue_overflow
                                                                                   : MessageErrorCode::unroutable);
            }
            if (completion) completion(ec);
        });
    });
}

std::vector<std::string> NodeRouter::return_route(const std::vector<std::string>& route, size_t consumed) {
    return std::vector<std::string>(route.rbegin() + (route.size() - consumed), route.rend());
}

bool NodeRouter::nonce_seen_and_insert(const std::string& source, uint32_t nonce) {
    std::shared_ptr<LruCache<uint32_t, char>> window;
    char hit = 0;
    bool stale = false;
    if (!nonces_->get(source, window, stale)) {
        window = std::make_shared<LruCache<uint32_t, char>>(options_.nonce_capacity, window_);
        nonces_->put(source, window);
    }
    if (window->get(nonce, hit, stale)) return true;
    window->put(nonce, 0);
    return false;
}

bool NodeRouter::message_id_seen_and_insert(uint64_t message_id) {
    char hit = 0;
    bool stale = false;
    if (seen_ids_->get(message_id, hit, stale)) return true;
    seen_ids_->put(message_id, 0);
    return false;
}

void NodeRouter::stop() {
    bool expected = false;
    if (!stopped_.compare_exchange_strong(expected, true)) return;
    auto self = shared_from_this();
    executor_->post([self] { self->links_.clear(); });
}

} // namespace pr::node
