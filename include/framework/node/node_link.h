#pragma once

#include "framework/net/native_transport.h"
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
// endian, zero for Hello/Ping/Pong/Notify), and arbitrary binary payload.
// The length counts the 10-byte header and payload, not its own four bytes.
// Unknown versions/types, invalid ids, short/oversize bodies and truncated
// frames are protocol errors and close the link. No padding or terminator.
enum class FrameType : uint8_t { hello = 1, ping = 2, pong = 3, request = 4, response = 5, notify = 6 };
struct Frame { FrameType type; uint64_t id = 0; std::string payload; };
std::string encode_frame(const Frame& frame);
bool decode_frame(const std::string& body, size_t max_frame_bytes, Frame& out);

struct LinkOptions {
    std::string node_id;
    std::string expected_peer_id; // empty accepts any nonempty peer id
    size_t max_frame_bytes = 1024 * 1024;
    size_t max_pending_requests = 128;
    size_t max_queued_frames = 128; // includes the current write
    std::chrono::milliseconds heartbeat_interval{30000};
    std::chrono::milliseconds heartbeat_timeout{10000};
    std::chrono::milliseconds reconnect_min{100};
    std::chrono::milliseconds reconnect_max{10000};
    net::ClientTlsConfig tls; // reserved: v0 always uses plain TCP
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

private:
    NodeLink(std::shared_ptr<net::IExecutor>, LinkOptions, bool client, TransportFactory);
    struct Pending { Completion completion; std::shared_ptr<net::ITimer> timer; };
    void begin_connect();
    void attach(std::shared_ptr<IConnection> connection);
    void read_next(uint64_t generation);
    void consume(const char* data, size_t size);
    void handle_frame(const Frame& frame);
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
    TransportFactory factory_;
    std::string host_;
    uint16_t port_ = 0;
    std::shared_ptr<IConnection> connection_;
    std::shared_ptr<net::ITimer> heartbeat_timer_, reconnect_timer_;
    std::map<uint64_t, Pending> pending_;
    std::deque<std::shared_ptr<std::string>> writes_;
    std::string incoming_, peer_id_;
    mutable std::mutex peer_mutex_;
    MessageHandler request_handler_, notify_handler_;
    StateHandler state_handler_;
    std::atomic<bool> ready_{false}, stopped_{false};
    std::atomic<uint64_t> next_id_{1}, attempts_{0};
    std::atomic<size_t> outstanding_{0}; // reserves capacity before posting to executor
    uint64_t generation_ = 0, reconnect_count_ = 0;
    bool hello_received_ = false, writing_ = false, awaiting_pong_ = false;
    std::chrono::steady_clock::time_point last_activity_;
    std::chrono::steady_clock::time_point ping_sent_;
};

} // namespace pr::node
