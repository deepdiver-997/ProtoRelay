#pragma once

#include "framework/net/native_transport.h"
#include "framework/node/node_trust.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pr::node {

// Wire format: 4-byte big-endian body length (10..max_frame_bytes), followed
// by version (1 byte, currently 1), type (1 byte), request id (8-byte big-
// endian, zero for every type except request/response), and arbitrary binary
// payload. The length counts the 10-byte header and payload, not its own four
// bytes. Unknown versions/types, invalid ids, short/oversize bodies and
// truncated frames are protocol errors and close the link. No padding or
// terminator.
//
// v1 control plane: hello_auth (8) completes the signed-challenge handshake
// and epoch_config (7) carries a controller-signed trust-set update. Both
// exist only on authenticated links (LinkOptions::trust set); a plain link
// treats them as protocol errors.
enum class FrameType : uint8_t {
    hello = 1, ping = 2, pong = 3, request = 4, response = 5, notify = 6,
    epoch_config = 7, hello_auth = 8
};
struct Frame { FrameType type; uint64_t id = 0; std::string payload; };
std::string encode_frame(const Frame& frame);
bool decode_frame(const std::string& body, size_t max_frame_bytes, Frame& out);

// Canonical epoch-config payload WITHOUT the trailing controller signature
// (control plane). The controller signs exactly the returned bytes and the
// sender appends the raw 64-byte signature; the receiving link verifies that
// binding before applying the update to its TrustStore.
std::string encode_epoch_update(uint64_t epoch, const std::vector<TrustStore::Neighbor>& neighbors,
                                const std::vector<std::string>& revoked);

// Crypto seam for the signed handshake, transport-independent and free of
// crypto-library types: sign returns the raw 64-byte Ed25519 signature over
// the message for this node's private key; verify checks (raw 32-byte public
// key, message, signature). Both must be set together with trust; the
// OpenSSL-backed implementation lives in the optional protorelay_node_auth
// module (node_auth.h).
struct NodeCredentials {
    std::function<std::string(const std::string&)> sign;
    std::function<bool(const std::string&, const std::string&, const std::string&)> verify;
};

struct LinkOptions {
    std::string node_id;
    std::string expected_peer_id; // empty accepts any peer present in trust
    size_t max_frame_bytes = 1024 * 1024;
    size_t max_pending_requests = 128;
    size_t max_queued_frames = 128; // includes the current write
    std::chrono::milliseconds heartbeat_interval{30000};
    std::chrono::milliseconds heartbeat_timeout{10000};
    std::chrono::milliseconds reconnect_min{100};
    std::chrono::milliseconds reconnect_max{10000};
    net::ClientTlsConfig tls; // used by the client when use_tls is set
    bool use_tls = false;     // client-side STARTTLS upgrade after TCP connect;
                              // server links accept whatever connection they are given
    // Authenticated mode: signed-challenge handshake against the neighbor
    // table. Identity lives at the frame layer (survives channel rebuilds,
    // works over plain TCP and TLS alike); TLS only encrypts the channel.
    std::shared_ptr<TrustStore> trust;
    NodeCredentials credentials;
};

class NodeLink final : public std::enable_shared_from_this<NodeLink> {
public:
    using Completion = std::function<void(std::error_code, std::string)>;
    using MessageHandler = std::function<void(uint64_t, const std::string&)>;
    using StateHandler = std::function<void(bool, std::error_code)>;
    using TransportFactory = std::function<std::shared_ptr<net::IClientTransport>()>;

    static std::shared_ptr<NodeLink> client(std::shared_ptr<net::IExecutor> executor,
                                            LinkOptions options, TransportFactory factory = {});
    static std::shared_ptr<NodeLink> accepted(std::unique_ptr<IConnection> connection,
                                              LinkOptions options);
    ~NodeLink();
    void start(const std::string& host, uint16_t port); // client only
    void stop(); // safe from any thread; completion/state callbacks stay on executor
    uint64_t request(std::string payload, std::chrono::milliseconds timeout, Completion completion);
    void cancel(uint64_t id);
    void respond(uint64_t id, std::string payload);
    void notify(std::string payload, std::function<void(std::error_code)> completion = {});
    void on_request(MessageHandler handler);
    void on_notify(MessageHandler handler);
    void on_state(StateHandler handler);
    bool ready() const { return ready_.load(); }
    uint64_t connect_attempts() const { return attempts_.load(); }
    std::string peer_id() const;
    // Peer's advertised config epoch (authenticated handshake only; 0 before).
    uint64_t peer_epoch() const;
    // Sends a controller-signed epoch-config payload (see node_trust.h) over
    // an established link. The payload is opaque here: the caller obtains and
    // signs it out of band; this link only transports and, on receipt, applies
    // it after verifying the controller signature.
    void send_epoch_config(std::string payload, std::function<void(std::error_code)> completion = {});

private:
    NodeLink(std::shared_ptr<net::IExecutor>, LinkOptions, bool client, TransportFactory);
    struct Pending { Completion completion; std::shared_ptr<net::ITimer> timer; };
    struct Challenge { std::string node_id; uint64_t epoch = 0; std::string nonce; };
    void begin_connect();
    void attach(std::shared_ptr<IConnection> connection);
    void read_next(uint64_t generation);
    void consume(const char* data, size_t size);
    void handle_frame(const Frame& frame);
    void handle_hello_challenge(const Frame& frame);
    void handle_hello_auth(const Frame& frame);
    void apply_epoch_config(const std::string& payload);
    void recheck_trust(const TrustStore::Snapshot& snapshot);
    void register_trust_listener();
    bool peer_acceptable(const std::string& node_id) const;
    bool seen_nonce(const std::string& nonce) const;
    void remember_nonce(std::string nonce);
    void set_peer(const std::string& node_id, uint64_t epoch);
    static std::string random_nonce();
    bool enqueue(Frame frame);
    void write_next(uint64_t generation);
    void tick(uint64_t generation);
    void disconnect(std::error_code error);
    void schedule_reconnect();
    void complete(uint64_t id, std::error_code error, std::string payload);
    void emit_state(bool state, std::error_code error);
    void stop_on_executor();

    std::shared_ptr<net::IExecutor> executor_;
    LinkOptions options_;
    bool client_;
    bool authenticated_;
    TransportFactory factory_;
    std::string host_;
    uint16_t port_ = 0;
    std::shared_ptr<IConnection> connection_;
    std::shared_ptr<net::ITimer> heartbeat_timer_, reconnect_timer_;
    std::map<uint64_t, Pending> pending_;
    std::deque<std::shared_ptr<std::string>> writes_;
    std::string incoming_, peer_id_;
    mutable std::mutex peer_mutex_;
    uint64_t peer_epoch_ = 0;
    MessageHandler request_handler_, notify_handler_;
    StateHandler state_handler_;
    std::atomic<bool> ready_{false}, stopped_{false};
    std::atomic<uint64_t> next_id_{1}, attempts_{0};
    std::atomic<size_t> outstanding_{0}; // reserves capacity before posting to executor
    uint64_t generation_ = 0, reconnect_count_ = 0;
    // Handshake state (executor-only). done replaces v0's hello_received_;
    // in authenticated mode it additionally covers the signed response.
    enum class AuthState : uint8_t { challenge, response, done };
    AuthState auth_state_ = AuthState::challenge;
    Challenge own_challenge_;  // what we sent this connection
    Challenge peer_challenge_; // what we received this connection
    bool writing_ = false, awaiting_pong_ = false, peer_revoked_ = false;
    std::deque<std::string> seen_nonces_; // replay guard, survives reconnects
    std::chrono::steady_clock::time_point last_activity_;
    std::chrono::steady_clock::time_point ping_sent_;
};

} // namespace pr::node
