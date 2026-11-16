#ifndef PR_FRAMEWORK_NODE_QUIC_LINK_H
#define PR_FRAMEWORK_NODE_QUIC_LINK_H
// QUIC-backed transport adapters for NodeLink — pure contract composition.
//
// This header only composes existing framework contracts: NodeLink's
// IClientTransport factory seam on one side, the mux contracts of
// quic_transport.h on the other. It never names the optional protorelay_quic
// module: the application injects the module's transports
// (pr::net::quic::make_quic_client_transport / make_quic_server_transport
// products) into QuicLinkEndpoint / QuicStreamListener, so framework-only and
// contracts-only builds compile this header unchanged while the module is
// absent.
//
// Layering (unchanged from the node design):
//   - identity stays at the frame layer (signed handshake, epoch revocation);
//     QUIC only encrypts the channel, embedded TLS 1.3, verification mandatory.
//   - LinkOptions::tls / use_tls are meaningless on QUIC-backed links: the
//     adapter's async_start_tls completes successfully because encryption is
//     constitutive — there is no plaintext path to downgrade to.
//
// Threading: every object here is bound to one executor and all its state is
// touched only while running on that executor. The caller satisfies this
// naturally: NodeLink creates transports and calls them from its own executor,
// and the module delivers every mux/stream completion to the executor given at
// construction. Handlers passed here always run posted (never inline) on that
// executor, exactly once. An endpoint keeps its muxes alive while any adapter
// lives (adapters hold the endpoint), so delivered streams stay valid for as
// long as their NodeLink holds the adapter.
#include "framework/net/quic_transport.h"
#include "framework/net/server_transport.h"   // TlsConfig complete type
#include <atomic>
#include <cassert>
#include <deque>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pr::node {

// ---------------------------------------------------------------------------
// Client side: one endpoint per dialing process, shared by any number of
// NodeLinks. Healthy muxes are reused: reconnecting a link opens a new stream
// on the existing QUIC connection instead of re-handshaking; a dead mux is
// redialed. Each NodeLink connection attempt gets its own adapter via
// make_stream_transport() (the factory_() discipline in NodeLink::begin_connect).
// ---------------------------------------------------------------------------
class QuicLinkEndpoint final : public std::enable_shared_from_this<QuicLinkEndpoint> {
public:
    // transport: product of pr::net::quic::make_quic_client_transport(executor).
    // executor: the same executor the NodeLinks will run on (NodeLink asserts
    // transport->executor() == its executor; mux/stream completions land there).
    // transport: product of pr::net::quic::make_quic_client_transport — the
    // contract factory hands this one out as shared_ptr (server side is unique_ptr).
    static std::shared_ptr<QuicLinkEndpoint> make(
        std::shared_ptr<net::IQuicClientTransport> transport,
        net::ClientTlsConfig tls, std::shared_ptr<net::IExecutor> executor) {
        return std::shared_ptr<QuicLinkEndpoint>(new QuicLinkEndpoint(
            std::move(transport), std::move(tls), std::move(executor)));
    }

    // NodeLink TransportFactory body: one fresh adapter per attempt.
    std::shared_ptr<net::IClientTransport> make_stream_transport() {
        return std::static_pointer_cast<net::IClientTransport>(
            std::make_shared<QuicStreamTransport>(this->shared_from_this()));
    }

private:
    using SlotKey = std::pair<std::string, uint16_t>;
    struct MuxSlot;

    class QuicStreamTransport final : public net::IClientTransport,
                                      public std::enable_shared_from_this<QuicStreamTransport> {
    public:
        explicit QuicStreamTransport(std::shared_ptr<QuicLinkEndpoint> endpoint)
            : endpoint_(std::move(endpoint)) {}

        std::shared_ptr<net::IExecutor> executor() override { return endpoint_->executor_; }

        // Dial (or reuse) the mux for host:port, then open one stream. The
        // stream is the connection; closing it does not close the mux.
        void async_connect(const std::string& host, uint16_t port,
                           HandshakeHandler handler) override {
            assert(!done_ && stream_ == nullptr && "one connect at a time");
            host_ = host; port_ = port; handler_ = std::move(handler);
            // 限定到自身 esft 基类：否则嵌套类作用域里会撞上外层
            // QuicLinkEndpoint 的 enable_shared_from_this 名字查找。
            endpoint_->request_stream(esft::shared_from_this());
        }

        // QUIC embeds TLS 1.3: the channel is already encrypted, so this is
        // the constitutive no-op. Posted, like every completion here.
        void async_start_tls(net::ClientTlsConfig, HandshakeHandler handler) override {
            assert(done_ && stream_ && "connect must complete first");
            auto self = esft::shared_from_this();
            endpoint_->executor_->post([self, handler = std::move(handler)] {
                handler(std::error_code());
            });
        }

        // ---- IConnection facade over the stream ----
        void async_read(net::MutableBuffer buffer, ReadHandler handler) override {
            assert(stream_ && "read before connect");
            stream_->async_read(buffer, std::move(handler));
        }
        void async_write(net::ConstBuffer buffer, WriteHandler handler) override {
            assert(stream_ && "write before connect");
            stream_->async_write(buffer, std::move(handler));
        }
        void async_handshake(net::HandshakeRole, HandshakeHandler handler) override {
            auto self = esft::shared_from_this();
            endpoint_->executor_->post([self, handler = std::move(handler)] {
                handler(std::error_code());
            });
        }
        void close() override {
            if (stream_) stream_->close();
        }
        void cancel() override {
            if (stream_) stream_->cancel();
        }
        bool is_open() const override { return stream_ && stream_->is_open(); }
        uint16_t get_local_port() const override { return 0; }
        std::string get_remote_ip() const override { return host_; }

    private:
        using esft = std::enable_shared_from_this<QuicStreamTransport>;
        friend class QuicLinkEndpoint;
        // Exactly once, always posted: the handler the NodeLink passed to
        // async_connect. On success the adapter owns the stream from here on.
        void settle(const std::error_code& error, std::unique_ptr<IConnection> stream) {
            if (done_) return;
            done_ = true;
            if (!error) stream_ = std::move(stream);
            auto self = esft::shared_from_this();
            endpoint_->executor_->post([self, handler = std::move(handler_), error] {
                handler(error);
            });
        }
        std::shared_ptr<QuicLinkEndpoint> endpoint_;
        HandshakeHandler handler_;
        std::unique_ptr<IConnection> stream_;
        std::string host_;
        uint16_t port_ = 0;
        bool done_ = false;
    };

    struct MuxSlot {
        std::unique_ptr<net::IMuxConnection> mux;
        bool opening = false;   // one pending open_stream per mux (contract)
        // Dead adapters are tolerated everywhere via expired weak_ptr; there
        // is deliberately no unregister path that would touch state off the
        // executor (NodeLink may drop an adapter from any thread).
        std::deque<std::weak_ptr<QuicStreamTransport>> waiters;
    };

    explicit QuicLinkEndpoint(std::shared_ptr<net::IQuicClientTransport> transport,
                              net::ClientTlsConfig tls, std::shared_ptr<net::IExecutor> executor)
        : transport_(std::move(transport)), tls_(std::move(tls)), executor_(std::move(executor)) {
        assert(transport_ && executor_ && transport_->executor() == executor_);
    }

    void request_stream(std::shared_ptr<QuicStreamTransport> adapter) {
        const SlotKey key{adapter->host_, adapter->port_};
        MuxSlot& slot = slots_[key];
        slot.waiters.push_back(adapter);
        // 去重：一个 key 同时至多一个排队/在途拨号。重复入队会在槽位被错误
        // 清空后经 slots_[key] 复活幽灵槽，吞掉后续真正的拨号。
        if (!slot.mux && !dial_keys_.count(key)) {
            dial_keys_.insert(key);
            dial_queue_.push_back(key);
        }
        pump();
    }

    void pump() {
        pump_dial();
        for (auto& [key, slot] : slots_) (void)slot, pump_opens(key);
    }

    // One dial in flight at a time (IQuicClientTransport contract); further
    // keys queue and are dialed as completions arrive.
    void pump_dial() {
        if (dial_pending_ || dial_queue_.empty()) return;
        const SlotKey key = dial_queue_.front();
        dial_queue_.pop_front();
        MuxSlot& slot = slots_[key];
        if (slot.mux) {   // healthy meanwhile: satisfied, drop the queued key
            dial_keys_.erase(key);
            pump_dial();
            return;
        }
        dial_pending_ = true;
        auto self = this->shared_from_this();
        transport_->async_connect(key.first, key.second, tls_,
            [self, key](const std::error_code& error, std::unique_ptr<net::IMuxConnection> mux) {
                self->dial_pending_ = false;
                self->dial_keys_.erase(key);
                auto it = self->slots_.find(key);
                if (it == self->slots_.end()) { self->pump_dial(); return; }
                if (error) {
                    self->fail_slot(it, error);
                } else {
                    it->second.mux = std::move(mux);
                    self->pump_opens(key);
                }
                self->pump_dial();
            });
    }

    // Hand queued waiters streams on this mux, one open_stream at a time.
    void pump_opens(const SlotKey& key) {
        auto it = slots_.find(key);
        if (it == slots_.end() || !it->second.mux || it->second.opening) return;
        MuxSlot& slot = it->second;
        while (!slot.opening && !slot.waiters.empty()) {
            auto adapter = slot.waiters.front().lock();
            slot.waiters.pop_front();
            if (!adapter) continue;   // abandoned attempt
            slot.opening = true;
            auto self = this->shared_from_this();
            slot.mux->open_stream([self, key, adapter](const std::error_code& error,
                                                       std::unique_ptr<IConnection> stream) {
                self->on_stream_opened(key, adapter, error, std::move(stream));
            });
            return;   // next waiter waits for this open to finish
        }
    }

    void on_stream_opened(const SlotKey& key, std::shared_ptr<QuicStreamTransport> adapter,
                          const std::error_code& error, std::unique_ptr<IConnection> stream) {
        auto it = slots_.find(key);
        if (it == slots_.end()) return;
        it->second.opening = false;
        if (error) {
            // open_stream failure means the connection ended either way:
            // settle the opener itself, then hand every queued waiter the same
            // error (the slot dies with the mux).
            adapter->settle(error, nullptr);
            fail_slot(it, error);
        } else {
            adapter->settle(error, std::move(stream));
            pump_opens(key);   // next queued waiter opens its own stream
        }
    }

    void fail_slot(std::map<SlotKey, MuxSlot>::iterator it, const std::error_code& error) {
        auto waiters = std::move(it->second.waiters);
        slots_.erase(it);
        for (auto& weak : waiters)
            if (auto adapter = weak.lock()) adapter->settle(error, nullptr);
    }

    std::shared_ptr<net::IQuicClientTransport> transport_;
    net::ClientTlsConfig tls_;
    std::shared_ptr<net::IExecutor> executor_;
    std::map<SlotKey, MuxSlot> slots_;
    std::set<SlotKey> dial_keys_;        // queued or in-flight dial keys (dedup)
    std::deque<SlotKey> dial_queue_;
    bool dial_pending_ = false;
};

// ---------------------------------------------------------------------------
// Server side: accept QUIC connections, pump every peer stream into a handler.
// The application turns each delivered stream into NodeLink::accepted(stream,
// options). Delivered streams are owned by the handler; stopping the listener
// leaves them untouched. Hold the shared_ptr for the listener's lifetime:
// module accept completions keep it alive across posted callbacks.
// ---------------------------------------------------------------------------
class QuicStreamListener final : public std::enable_shared_from_this<QuicStreamListener> {
public:
    using StreamHandler = std::function<void(std::unique_ptr<IConnection>)>;

    // transport: product of pr::net::quic::make_quic_server_transport(config, executor).
    // executor: the same one given to that factory — the QUIC engine mutates
    // its tick timer there, so listener start/stop are posted onto it (the
    // QUIC server transport itself does not expose executor()).
    static std::shared_ptr<QuicStreamListener> make(
        std::unique_ptr<net::IQuicServerTransport> transport, net::TlsConfig tls,
        std::shared_ptr<net::IExecutor> executor) {
        return std::shared_ptr<QuicStreamListener>(new QuicStreamListener(
            std::move(transport), std::move(tls), std::move(executor)));
    }

    const net::QuicServerConfig& config() const { return transport_->config(); }

    void start(StreamHandler on_stream) {
        assert(on_stream && !started_);
        started_ = true;
        on_stream_ = std::move(on_stream);
        auto self = this->shared_from_this();
        executor_->post([self] {
            self->transport_->configure_tls(self->tls_);
            self->transport_->start([self](std::unique_ptr<net::IMuxConnection> mux) {
                self->on_mux(std::move(mux));
            });
        });
    }

    // Stops listening and cancels pending accepts; the shutdown itself runs on
    // the executor (QUIC engine/timer affinity). Already-delivered streams
    // (with the NodeLinks on them) are untouched. stopped_ is atomic because
    // the caller thread flips it while the loop reads it.
    void stop() {
        if (!started_ || stopped_.exchange(true)) return;
        auto self = this->shared_from_this();
        executor_->post([self] {
            self->muxes_.clear();   // mux dtor cancels pending accepts
            self->transport_->stop();
            // The listener is held by the accept callbacks it installed
            // (transport handler + on_stream): clearing both here is what lets
            // the listener, its transport and their executor die after stop().
            self->on_stream_ = {};
        });
    }

private:
    explicit QuicStreamListener(std::unique_ptr<net::IQuicServerTransport> transport,
                                net::TlsConfig tls, std::shared_ptr<net::IExecutor> executor)
        : transport_(std::move(transport)), tls_(std::move(tls)), executor_(std::move(executor)) {
        assert(executor_);
    }

    void on_mux(std::unique_ptr<net::IMuxConnection> mux) {
        if (stopped_) return;
        muxes_.push_back(std::move(mux));
        pump_accept(muxes_.back().get());
    }

    // One pending accept_stream per mux (contract); rearm after each delivery.
    // `mux` is the pointee address, stable across the owning vector's moves.
    void pump_accept(net::IMuxConnection* mux) {
        auto self = this->shared_from_this();
        mux->accept_stream([self, mux](const std::error_code& error,
                                       std::unique_ptr<IConnection> stream) {
            if (self->stopped_) return;
            if (error == std::errc::operation_canceled) return;   // stop() cleared muxes
            if (error) return;   // mux ended: dead entry, no further accepts
            self->on_stream_(std::move(stream));
            self->pump_accept(mux);
        });
    }

    std::unique_ptr<net::IQuicServerTransport> transport_;
    net::TlsConfig tls_;
    std::shared_ptr<net::IExecutor> executor_;
    StreamHandler on_stream_;
    std::vector<std::unique_ptr<net::IMuxConnection>> muxes_;
    bool started_ = false;
    std::atomic<bool> stopped_{false};
};

} // namespace pr::node

#endif // PR_FRAMEWORK_NODE_QUIC_LINK_H
