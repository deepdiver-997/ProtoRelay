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
    if (type < 1 || type > 6) return false;
    const auto id = get64(body.data() + 2);
    if ((type == 4 || type == 5) ? id == 0 : id != 0) return false;
    out = {static_cast<FrameType>(type), id, body.substr(10)};
    return true;
}

NodeLink::NodeLink(std::shared_ptr<net::IExecutor> executor, LinkOptions options,
                   bool client, TransportFactory factory)
    : executor_(std::move(executor)), options_(std::move(options)), client_(client),
      factory_(std::move(factory)) {
    if (!executor_ || options_.node_id.empty() || options_.max_frame_bytes < 10 ||
        options_.max_frame_bytes > std::numeric_limits<uint32_t>::max() ||
        options_.max_pending_requests == 0 || options_.max_queued_frames == 0 ||
        options_.heartbeat_interval.count() <= 0 || options_.heartbeat_timeout.count() <= 0 ||
        options_.reconnect_min.count() <= 0 || options_.reconnect_max < options_.reconnect_min ||
        options_.node_id.size() > options_.max_frame_bytes - 10 ||
        !options_.tls.server_name.empty() || !options_.tls.ca_file.empty() ||
        !options_.tls.ca_pem.empty() || !options_.tls.use_system_ca)
        throw std::invalid_argument("invalid node link options");
}
NodeLink::~NodeLink() = default;
std::string NodeLink::peer_id() const {
    std::lock_guard<std::mutex> lock(peer_mutex_);
    return peer_id_;
}
std::shared_ptr<NodeLink> NodeLink::client(std::shared_ptr<net::IExecutor> executor,
                                           LinkOptions options, TransportFactory factory) {
    if (!factory) factory = [executor] { return net::native::make_native_client_transport(executor); };
    return std::shared_ptr<NodeLink>(new NodeLink(std::move(executor), std::move(options), true, std::move(factory)));
}
std::shared_ptr<NodeLink> NodeLink::accepted(std::unique_ptr<IConnection> connection, LinkOptions options) {
    if (!connection) throw std::invalid_argument("missing connection");
    auto executor = connection->executor();
    auto link = std::shared_ptr<NodeLink>(new NodeLink(executor, std::move(options), false, {}));
    auto shared = std::shared_ptr<IConnection>(std::move(connection));
    executor->post([link, shared] { link->attach(shared); });
    return link;
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
        self->attach(transport);
    });
}
void NodeLink::attach(std::shared_ptr<IConnection> connection) {
    if (stopped_) { connection->close(); return; }
    connection_ = std::move(connection);
    ++generation_;
    incoming_.clear();
    { std::lock_guard<std::mutex> lock(peer_mutex_); peer_id_.clear(); }
    hello_received_ = false; awaiting_pong_ = false;
    writing_ = false; writes_.clear();
    last_activity_ = std::chrono::steady_clock::now();
    if (!enqueue({FrameType::hello, 0, options_.node_id})) { disconnect(err(std::errc::no_buffer_space)); return; }
    read_next(generation_);
    tick(generation_);
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
    if (!hello_received_) {
        if (frame.type != FrameType::hello || frame.payload.empty() ||
            (!options_.expected_peer_id.empty() && frame.payload != options_.expected_peer_id)) {
            disconnect(err(std::errc::permission_denied)); return;
        }
        { std::lock_guard<std::mutex> lock(peer_mutex_); peer_id_ = frame.payload; }
        hello_received_ = true; reconnect_count_ = 0;
        ready_ = true; emit_state(true, {}); return;
    }
    if (frame.type == FrameType::hello) { disconnect(err(std::errc::protocol_error)); return; }
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
    if (!hello_received_) {
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
    { std::lock_guard<std::mutex> lock(peer_mutex_); peer_id_.clear(); }
    ++generation_;
    if (heartbeat_timer_) { heartbeat_timer_->cancel(); heartbeat_timer_.reset(); }
    connection_->close(); connection_.reset();
    writes_.clear(); writing_ = false; incoming_.clear(); awaiting_pong_ = false;
    while (!pending_.empty()) complete(pending_.begin()->first, error, {});
    if (was_ready || error) emit_state(false, error);
    if (client_ && !stopped_) schedule_reconnect();
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
