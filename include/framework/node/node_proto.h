#pragma once

// Node message protocol (v1): end-to-end envelopes, controlled source routing
// and bounded forwarding on top of NodeLink's point-to-point frames. The
// router is a dumb pipe for foreign destinations: it only reads routing
// fields, never decrypts payloads, and knows nothing beyond its neighbor
// table. Control-plane messages (signed epoch-config) are out of scope here.

#include "framework/cache/lru_cache.h"
#include "framework/node/node_link.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pr::node {

// Message types. probe is auto-answered by the destination router with an
// ack (payload = [u64 acked message id][echoed probe payload]) and is not
// delivered to the application; data/ack are plain application messages.
enum class MessageType : uint8_t { data = 1, ack = 2, error = 3, probe = 4 };

// Failure codes carried in error envelopes as payload [u8 code][u64 failed
// message id]. Error envelopes are never encrypted, never re-reported (a
// failed error is silently dropped) and never re-delivered by the router.
enum class MessageErrorCode : uint8_t {
    none = 0,
    unroutable = 1,      // next hop unknown, off-path arrival, link failure
    ttl_exhausted = 2,   // hop budget reached zero before delivery
    replay_rejected = 3, // nonce window or timestamp tolerance violation
    decrypt_failed = 4,  // encrypted payload missing key / failed AEAD tag
    unknown_type = 5,    // known envelope version, unregistered type
    queue_overflow = 6,  // bounded in-flight queue rejected the message
};

// End-to-end message envelope; travels as the payload of one NodeLink notify
// frame. Wire layout (all integers big-endian):
//   [u8 version=1][u8 type][u8 flags][u8 key_id][u32 ttl][u32 nonce]
//   [u64 timestamp_ms][u64 message_id]
//   [u16 src_len][source][u16 dst_len][destination]
//   [u8 route_len][u8 route_pos]{route_len x [u16 len][hop id]}
//   [u32 payload_len][payload]
// flags bit0 marks the payload as AEAD ciphertext (AES-256-GCM where built
// with OpenSSL; the 12-byte nonce is message_id||nonce, the header up to the
// payload is the AAD); bits1..7 are reserved and must be zero. ttl counts the
// forwards still allowed; route lists intermediate relays (excluding source
// and destination); route_pos is the number of route entries already
// consumed. Envelope.payload holds plaintext in the router API; the codec
// passes payload bytes through verbatim in both directions.
struct Envelope {
    uint8_t version = 1;
    MessageType type = MessageType::data;
    bool encrypted = false;
    uint8_t key_id = 0;
    uint32_t ttl = 0;          // 0 on send(): router fills max_hops
    uint32_t nonce = 0;        // 0 on send(): router fills a random value
    uint64_t timestamp_ms = 0; // 0 on send(): router fills the current time
    uint64_t message_id = 0;   // 0 on send(): router fills a random value
    std::string source, destination;
    std::vector<std::string> route;
    uint8_t route_pos = 0;
    std::string payload;
};

struct NodeRouterOptions {
    size_t max_envelope_bytes = 256 * 1024; // must also fit every link frame
    size_t max_node_id_bytes = 255;
    size_t max_route_hops = 8;
    uint32_t max_hops = 8; // ttl written by send(); also used by error reports
    std::chrono::milliseconds timestamp_tolerance{30000};
    size_t max_in_flight = 128;          // messages handed to links, not yet flushed
    size_t seen_message_capacity = 4096; // message-id loop/dedup cache
    size_t nonce_capacity = 4096;        // per-source nonce window entries
    size_t max_tracked_sources = 256;    // bounded number of nonce windows
    bool dedup_by_message_id = true;     // receiver-side end-to-end dedup
    bool forward_enabled = true;
};

// malformed: structure/bounds violated — no field can be trusted (including
// source), so the message is silently dropped, never reported, and the link
// stays up. unknown_version: an unknown version byte may come with any
// layout, so those envelopes are dropped for the same reason. ok_unknown_
// type: v1 structure, unregistered type — the router reports it to the source.
enum class DecodeResult { ok, ok_unknown_type, malformed, unknown_version };
std::string encode_envelope(const Envelope& envelope);
DecodeResult decode_envelope(const std::string& wire, const NodeRouterOptions& limits, Envelope& out);

class NodeRouter final : public std::enable_shared_from_this<NodeRouter> {
public:
    using ReceiveHandler = std::function<void(const Envelope&)>;
    using ErrorReportHandler =
        std::function<void(const Envelope& report, MessageErrorCode code, uint64_t failed_message_id)>;
    using SendCompletion = std::function<void(std::error_code)>;

    static std::shared_ptr<NodeRouter> create(std::shared_ptr<net::IExecutor> executor,
                                              std::string node_id, NodeRouterOptions options = {});
    ~NodeRouter();
    NodeRouter(const NodeRouter&) = delete;
    NodeRouter& operator=(const NodeRouter&) = delete;

    const std::string& node_id() const { return node_id_; }
    const NodeRouterOptions& options() const { return options_; }
    size_t in_flight() const { return in_flight_.load(); }

    // Pre-shared end-to-end key for encrypted payloads (32 bytes for
    // AES-256-GCM). Without an AEAD build this is a validated no-op and
    // encrypted sends fail with errc::not_supported.
    void add_key(uint8_t key_id, const std::string& key);

    // neighbor_id must match the link's expected peer id. The router both
    // sends to and receives from this id over the link; both ends of an edge
    // attach their own end. Re-attaching an id replaces the link.
    void attach(const std::string& neighbor_id, std::shared_ptr<NodeLink> link);
    void detach(const std::string& neighbor_id);

    // data/ack envelopes whose destination is this node (decrypted). Handlers
    // run on the router executor.
    void on_message(ReceiveHandler handler);
    void on_error_report(ErrorReportHandler handler);

    // Sends a fresh message: fills ttl/nonce/timestamp/message_id when zero,
    // sets source, resets route_pos. The completion means "accepted by the
    // first-hop link queue", not end-to-end delivery — use an ack or a probe
    // round-trip for that. Errors: invalid_argument (malformed route/ids),
    // message_size, not_supported (encrypted without AEAD/key), host_
    // unreachable (first hop is not a neighbor), no_buffer_space (queue
    // bound), operation_canceled (stopped).
    void send(Envelope message, SendCompletion completion);
    void stop(); // idempotent; completions for unsent messages: operation_canceled

private:
    // Minimal state needed to send an error report back toward the original
    // source: its id, the route back (already reversed, excluding both ends),
    // the failed message id, and whether failures must stay silent (error
    // envelopes and router-generated acks never trigger further reports).
    struct ReturnPath {
        std::string source;
        std::vector<std::string> route;
        uint64_t message_id = 0;
        bool silent = false;
    };
    NodeRouter(std::shared_ptr<net::IExecutor> executor, std::string node_id, NodeRouterOptions options);
    void run_send(Envelope message, SendCompletion completion);
    void send_fresh(Envelope& message); // fill source/ttl/nonce/timestamp/message_id/route_pos
    void receive(const std::string& wire);
    void deliver(Envelope envelope);
    void forward(Envelope envelope);
    void report(const ReturnPath& back, MessageErrorCode code);
    // Router-generated reports and acks pass is_report=true: they bypass the
    // in-flight bound (a saturated queue must still be able to report) and
    // are bounded by the link queues instead.
    void handoff(const std::string& next_hop, std::string wire, ReturnPath back, bool is_report,
                 SendCompletion completion);
    static std::vector<std::string> return_route(const std::vector<std::string>& route, size_t consumed);
    bool nonce_seen_and_insert(const std::string& source, uint32_t nonce);
    bool message_id_seen_and_insert(uint64_t message_id);

    std::shared_ptr<net::IExecutor> executor_;
    std::string node_id_;
    NodeRouterOptions options_;
    std::map<std::string, std::shared_ptr<NodeLink>> links_;
    std::map<uint8_t, std::string> keys_;
    ReceiveHandler message_handler_;
    ErrorReportHandler error_handler_;
    // Per-source bounded nonce window: source -> recent nonces. Capacity
    // eviction bounds the state at max_tracked_sources x nonce_capacity.
    std::unique_ptr<LruCache<std::string, std::shared_ptr<LruCache<uint32_t, char>>>> nonces_;
    // Loop protection (always on) and end-to-end dedup (options_ flag) share
    // one bounded cache of recently seen message ids.
    std::unique_ptr<LruCache<uint64_t, char>> seen_ids_;
    std::chrono::seconds window_{60}; // replay-window retention: >= 2x tolerance
    std::atomic<size_t> in_flight_{0};
    std::atomic<bool> stopped_{false};
};

} // namespace pr::node
