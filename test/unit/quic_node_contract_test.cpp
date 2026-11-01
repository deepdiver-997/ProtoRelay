// quic_node_contract_test — QUIC↔NodeLink 适配层（quic_link.h）的确定性单测。
//
// 无 protorelay_quic 模块依赖：Fake mux/stream/transport 实现 quic_transport.h
// 契约，ManualExecutor 全程确定性驱动，无真实时延。覆盖：
//   S1 拨号→mux→open_stream→settle 全链 + start_tls 构成性 no-op + IConnection 转发
//   S2 mux 健康复用：第二条适配器同 key 零重拨，各开一条流
//   S3 拨号失败：错误透传；同 key 排队 waiter 全部收到同一错误
//   S4 open_stream 失败（mux 死亡）：槽位清空 + 全部 waiter 收 end_of_stream
//   S5 被遗弃的 waiter（adapter 已析构）：跳过不炸，后继 waiter 正常
//   S6 NodeLink 整链（桥接 FakeStream 对）：plain 握手 ready + request/response
//      + 第二条链路复用 mux（dial 1 次、open 2 次）+ 链路关闭后第三条链照常
// 服务侧 QuicStreamListener 的泵行为在 S6 一并覆盖（deliver_mux → accept 武装 →
// 逐流交付 → stop 取消挂起 accept）。
#undef NDEBUG
#include "manual_executor.h"
#include "framework/node/node_link.h"
#include "framework/node/quic_link.h"
#include "framework/net/quic_transport.h"
#include "framework/net/server_transport.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using namespace pr;                       // NOLINT
using namespace pr::net;                  // NOLINT
using namespace pr::node;                 // NOLINT
using mail_system::test::ManualExecutor;  // NOLINT

namespace {

// ── FakeStream：可桥接的字节流（全双工经 peer_ 互投） ─────────────
class FakeStream final : public IConnection {
public:
    explicit FakeStream(std::shared_ptr<ManualExecutor> ex) : ex_(std::move(ex)) {}

    std::shared_ptr<IExecutor> executor() override { return ex_; }

    void async_read(MutableBuffer buf, ReadHandler h) override {
        std::error_code ec;
        size_t n = 0;
        {
            std::lock_guard<std::mutex> lk(mu_);
            assert(!pending_read_.h && "overlapping reads");
            if (!inbuf_.empty()) {
                n = std::min(inbuf_.size(), buf.size());
                std::memcpy(buf.data(), inbuf_.data(), n);
                inbuf_.erase(0, n);
            } else {
                pending_read_ = Pending{buf, std::move(h)};
                return;
            }
        }
        ex_->post([h = std::move(h), ec, n] { h(ec, n); });
    }

    void async_write(ConstBuffer buf, WriteHandler h) override {
        std::string data(static_cast<const char*>(buf.data()), buf.size());
        FakeStream* peer = nullptr;
        {
            std::lock_guard<std::mutex> lk(mu_);
            out_ += data;
            peer = peer_;
        }
        if (peer) {
            ex_->post([peer, data] { peer->inject(data); });
        }
        ex_->post([h = std::move(h), n = data.size()] { h(std::error_code(), n); });
    }

    void async_handshake(HandshakeRole, HandshakeHandler h) override {
        ex_->post([h = std::move(h)] { h(std::error_code()); });
    }
    void close() override {
        const auto ec = std::make_error_code(std::errc::operation_canceled);
        ReadHandler h;
        {
            std::lock_guard<std::mutex> lk(mu_);
            closed_ = true;
            if (pending_read_.h) { h = std::move(pending_read_.h); pending_read_ = Pending{}; }
        }
        if (h) ex_->post([h = std::move(h), ec] { h(ec, 0); });
    }
    void cancel() override { close(); }
    bool is_open() const override { return !closed_; }
    uint16_t get_local_port() const override { return 0; }
    std::string get_remote_ip() const override { return "127.0.0.1"; }

    // ── 测试控制 ──
    void bridge(FakeStream* peer) { peer_ = peer; peer->peer_ = this; }
    std::string written() const {
        std::lock_guard<std::mutex> lk(mu_);
        return out_;
    }
    void inject(const std::string& data) {
        std::error_code ec;
        size_t n = 0;
        ReadHandler h;
        {
            std::lock_guard<std::mutex> lk(mu_);
            inbuf_ += data;
            if (!pending_read_.h) return;   // 数据留在缓冲，等下一次 async_read
            n = std::min(inbuf_.size(), pending_read_.buf.size());
            std::memcpy(pending_read_.buf.data(), inbuf_.data(), n);
            inbuf_.erase(0, n);
            h = std::move(pending_read_.h);
            pending_read_ = Pending{};
        }
        ex_->post([h = std::move(h), ec, n] { h(ec, n); });
    }

private:
    struct Pending { MutableBuffer buf; ReadHandler h; };
    std::shared_ptr<ManualExecutor> ex_;
    mutable std::mutex mu_;
    std::string inbuf_, out_;
    Pending pending_read_;
    FakeStream* peer_ = nullptr;
    bool closed_ = false;
};

// ── FakeMux：open/accept 挂起 + 手动交付 ─────────────────────────
class FakeMux final : public IMuxConnection {
public:
    explicit FakeMux(std::shared_ptr<ManualExecutor> ex) : ex_(std::move(ex)) {}

    std::shared_ptr<IExecutor> executor() const override { return ex_; }
    void open_stream(StreamHandler handler) override {
        assert(!open_pending_ && "one pending open");
        ++open_calls_;
        open_pending_ = true;
        open_handler_ = std::move(handler);
    }
    void accept_stream(StreamHandler handler) override {
        assert(!accept_pending_ && "one pending accept");
        accept_pending_ = true;
        accept_handler_ = std::move(handler);
    }
    MuxCloseInfo close_reason() const override { return {}; }
    void close() override { cancel(); }
    void cancel() override {
        const auto ec = std::make_error_code(std::errc::operation_canceled);
        if (open_pending_) {
            open_pending_ = false;
            ex_->post([h = std::move(open_handler_), ec] { h(ec, nullptr); });
        }
        if (accept_pending_) {
            accept_pending_ = false;
            ex_->post([h = std::move(accept_handler_), ec] { h(ec, nullptr); });
        }
    }
    bool is_open() const override { return true; }

    void deliver_open(const std::error_code& ec, std::unique_ptr<IConnection> stream) {
        assert(open_pending_);
        open_pending_ = false;
        auto box = std::make_shared<std::unique_ptr<IConnection>>(std::move(stream));
        ex_->post([h = std::move(open_handler_), ec, box] {
            h(ec, std::move(*box));
        });
    }
    void deliver_accept(std::unique_ptr<IConnection> stream) {
        assert(accept_pending_);
        accept_pending_ = false;
        auto box = std::make_shared<std::unique_ptr<IConnection>>(std::move(stream));
        ex_->post([h = std::move(accept_handler_), box] {
            h(std::error_code(), std::move(*box));
        });
    }

    int open_calls_ = 0;
    bool accept_pending() const { return accept_pending_; }

private:
    std::shared_ptr<ManualExecutor> ex_;
    StreamHandler open_handler_, accept_handler_;
    bool open_pending_ = false, accept_pending_ = false;
};

// ── Fake 拨号器 / 监听器 ─────────────────────────────────────────
class FakeQuicClient final : public IQuicClientTransport {
public:
    explicit FakeQuicClient(std::shared_ptr<ManualExecutor> ex) : ex_(std::move(ex)) {}
    std::shared_ptr<IExecutor> executor() const override { return ex_; }
    void async_connect(const std::string& host, uint16_t port, const ClientTlsConfig&,
                       ConnectHandler handler) override {
        assert(!connect_handler_ && "one dial at a time");
        ++dial_calls_;
        connect_host_ = host;
        connect_port_ = port;
        connect_handler_ = std::move(handler);
    }
    void deliver_connect(const std::error_code& ec, std::unique_ptr<IMuxConnection> mux) {
        assert(connect_handler_ && "no dial pending");
        auto box = std::make_shared<std::unique_ptr<IMuxConnection>>(std::move(mux));
        ex_->post([h = std::move(connect_handler_), ec, box] {
            h(ec, std::move(*box));
        });
    }
    int dial_calls_ = 0;
    bool has_pending_dial() const { return connect_handler_ != nullptr; }
    std::string connect_host_;
    uint16_t connect_port_ = 0;

private:
    std::shared_ptr<ManualExecutor> ex_;
    ConnectHandler connect_handler_;
};

class FakeQuicServer final : public IQuicServerTransport {
public:
    explicit FakeQuicServer(std::shared_ptr<ManualExecutor> ex) : ex_(std::move(ex)) {
        config_.address = "127.0.0.1";
        config_.port = 42424;
    }
    const QuicServerConfig& config() const override { return config_; }
    void configure_tls(const TlsConfig&) override { tls_applied_ = true; }
    void start(MuxAcceptHandler handler) override {
        assert(!started_);
        started_ = true;
        accept_handler_ = std::move(handler);
    }
    void stop() override { stopped_ = true; }
    void deliver_mux(std::unique_ptr<IMuxConnection> mux) {
        assert(started_ && !stopped_ && accept_handler_);
        auto box = std::make_shared<std::unique_ptr<IMuxConnection>>(std::move(mux));
        ex_->post([h = accept_handler_, box] { h(std::move(*box)); });
    }
    bool tls_applied_ = false, started_ = false, stopped_ = false;

private:
    std::shared_ptr<ManualExecutor> ex_;
    QuicServerConfig config_;
    MuxAcceptHandler accept_handler_;
};

LinkOptions plain_options(std::string id, std::string expected = {}) {
    LinkOptions o;
    o.node_id = std::move(id);
    o.expected_peer_id = std::move(expected);
    o.heartbeat_interval = std::chrono::milliseconds(60000);   // ManualTimer 不自动触发
    o.heartbeat_timeout = std::chrono::milliseconds(60000);
    o.reconnect_min = std::chrono::milliseconds(10);
    o.reconnect_max = std::chrono::milliseconds(20);
    return o;
}

} // namespace

// ── S1..S5：适配器语义（不含 NodeLink） ──────────────────────────
static void test_adapter_semantics() {
    auto ex = std::make_shared<ManualExecutor>();
    auto client = std::make_shared<FakeQuicClient>(ex);
    auto endpoint = QuicLinkEndpoint::make(client, ClientTlsConfig{}, ex);

    // S1 拨号 → mux → open → settle + start_tls no-op + 读写转发
    std::error_code s1_ec = std::make_error_code(std::errc::invalid_argument);
    bool s1_done = false;
    auto a1 = endpoint->make_stream_transport();
    a1->async_connect("h", 1, [&](const std::error_code& ec) { s1_ec = ec; s1_done = true; });
    ex->drain();
    assert(client->dial_calls_ == 1 && client->connect_host_ == "h" && client->connect_port_ == 1);
    auto mux_a = std::make_unique<FakeMux>(ex);
    auto* mux_a_raw = mux_a.get();
    client->deliver_connect({}, std::move(mux_a));
    ex->drain();
    assert(mux_a_raw->open_calls_ == 1 && !s1_done);   // open 还在途
    auto stream_a = std::make_unique<FakeStream>(ex);
    auto* stream_a_raw = stream_a.get();
    mux_a_raw->deliver_open({}, std::move(stream_a));
    ex->drain();
    assert(s1_done && !s1_ec && a1->is_open());
    a1->async_start_tls(ClientTlsConfig{}, [](const std::error_code& ec) { assert(!ec); });
    ex->drain();
    char chunk[8] = {};
    stream_a_raw->inject("x");
    a1->async_read(buffer(chunk, sizeof(chunk)), [](const std::error_code& ec, size_t n) { assert(!ec && n == 1); });
    ex->drain();
    assert(chunk[0] == 'x');
    a1->async_write(buffer("w", 1), [](const std::error_code&, size_t) {});
    ex->drain();
    assert(stream_a_raw->written() == "w");

    // S2 同 key 第二条：零重拨、各开一条流
    auto a2 = endpoint->make_stream_transport();
    bool s2_done = false;
    a2->async_connect("h", 1, [&](const std::error_code& ec) { assert(!ec); s2_done = true; });
    ex->drain();
    assert(client->dial_calls_ == 1 && !s2_done);   // 复用 mux A，无新拨
    mux_a_raw->deliver_open({}, std::make_unique<FakeStream>(ex));
    ex->drain();
    assert(s2_done && mux_a_raw->open_calls_ == 2);

    // S3 拨号失败透传
    auto a3 = endpoint->make_stream_transport();
    std::error_code s3_ec = std::make_error_code(std::errc::invalid_argument);
    a3->async_connect("h", 2, [&](const std::error_code& ec) { s3_ec = ec; });
    ex->drain();
    client->deliver_connect(std::make_error_code(std::errc::connection_refused), nullptr);
    ex->drain();
    assert(s3_ec == std::errc::connection_refused);

    // S3b 同 key 排队 waiter 全收同一错误（只拨一次）
    int refused_count = 0;
    const auto expect_refused = [&](const std::error_code& ec) {
        assert(ec == std::errc::connection_refused);
        ++refused_count;
    };
    auto a4 = endpoint->make_stream_transport();
    auto a5 = endpoint->make_stream_transport();
    a4->async_connect("h", 3, expect_refused);
    a5->async_connect("h", 3, expect_refused);
    ex->drain();
    assert(client->dial_calls_ == 3);   // a4/a5 同槽只拨一次（S1=1、S3=2 之后）
    client->deliver_connect(std::make_error_code(std::errc::connection_refused), nullptr);
    ex->drain();
    assert(refused_count == 2);

    // S4 open 失败（mux 死亡）：waiter 全收 end_of_stream，槽位清空
    int eof_count = 0;
    const auto expect_eof = [&](const std::error_code& ec) {
        assert(ec == IoErrc::end_of_stream);
        ++eof_count;
    };
    auto mux_b = std::make_unique<FakeMux>(ex);
    auto* mux_b_raw = mux_b.get();
    auto a6 = endpoint->make_stream_transport();
    auto a7 = endpoint->make_stream_transport();
    a6->async_connect("h", 4, expect_eof);
    a7->async_connect("h", 4, expect_eof);
    ex->drain();
    client->deliver_connect({}, std::move(mux_b));
    ex->drain();
    assert(mux_b_raw->open_calls_ == 1);   // 第一个 waiter 已发起 open
    mux_b_raw->deliver_open(make_error_code(IoErrc::end_of_stream), nullptr);
    ex->drain();
    assert(eof_count == 2);   // open 失败 → 槽位清空 → a7 同收 end_of_stream

    // S5 被遗弃的 waiter：跳过不炸，后继正常
    auto mux_c = std::make_unique<FakeMux>(ex);
    auto* mux_c_raw = mux_c.get();
    auto a8 = endpoint->make_stream_transport();
    a8->async_connect("h", 5, [](const std::error_code&) { assert(false); });
    ex->drain();
    a8.reset();   // NodeLink 弃置尝试（析构不走任何端点状态）
    client->deliver_connect({}, std::move(mux_c));
    ex->drain();
    assert(mux_c_raw->open_calls_ == 0);   // 死 waiter 被跳过，无 open
    auto a9 = endpoint->make_stream_transport();
    bool s9_done = false;
    a9->async_connect("h", 5, [&](const std::error_code& ec) { assert(!ec); s9_done = true; });
    ex->drain();
    assert(client->dial_calls_ == 5 && mux_c_raw->open_calls_ == 1 && !s9_done);
    mux_c_raw->deliver_open({}, std::make_unique<FakeStream>(ex));
    ex->drain();
    assert(s9_done);
}

// ── S6：NodeLink 整链（plain 握手 + 请求响应 + mux 复用 + 泵行为） ──
static void test_node_link_over_quic() {
    auto ex = std::make_shared<ManualExecutor>();
    auto client = std::make_shared<FakeQuicClient>(ex);
    auto server_u = std::make_unique<FakeQuicServer>(ex);
    auto* server = server_u.get();
    auto endpoint = QuicLinkEndpoint::make(client, ClientTlsConfig{}, ex);
    auto listener = QuicStreamListener::make(std::move(server_u), TlsConfig{}, ex);

    std::vector<std::shared_ptr<NodeLink>> server_links;
    listener->start([&](std::unique_ptr<IConnection> stream) {
        auto link = NodeLink::accepted(std::move(stream), plain_options("server", "client"));
        link->on_request([w = std::weak_ptr<NodeLink>(link)](uint64_t id, const std::string& payload) {
            if (auto l = w.lock()) l->respond(id, payload + "!");
        });
        server_links.push_back(std::move(link));
    });
    ex->drain();
    assert(server->tls_applied_ && server->started_);

    // 服务侧 mux 交付一次，accept 泵武装；之后每条 peer 流经 deliver_accept
    auto mux_s = std::make_unique<FakeMux>(ex);
    auto* mux_s_raw = mux_s.get();
    server->deliver_mux(std::move(mux_s));
    ex->drain();

    auto factory = [&endpoint] { return endpoint->make_stream_transport(); };
    std::vector<FakeMux*> client_mux_raws;   // 端点持所有权；这里只留驱动用观察指针
    std::vector<std::unique_ptr<FakeStream>> streams;   // 保活，防桥接悬垂
    // 返回 (link, 开流所在 mux)。首条链路走新拨（测试交付 mux），后续链路复用
    // 既有健康 mux（零交付——这正是适配器的复用语义）。
    auto bring_up = [&]() {
        auto link = NodeLink::client(ex, plain_options("client", "server"), factory);
        bool ready_seen = false;
        link->on_state([&](bool up, const std::error_code&) { if (up) ready_seen = true; });
        link->start("127.0.0.1", 42424);
        ex->drain();   // factory → （可能）拨号发起
        FakeMux* mux_raw = nullptr;
        if (client->has_pending_dial()) {
            auto mux = std::make_unique<FakeMux>(ex);
            mux_raw = mux.get();
            client_mux_raws.push_back(mux_raw);
            client->deliver_connect({}, std::move(mux));   // 所有权交给端点
        } else {
            mux_raw = client_mux_raws.front();   // 复用：同 key 健康 mux，零重拨
        }
        auto client_stream = std::make_unique<FakeStream>(ex);
        auto server_stream = std::make_unique<FakeStream>(ex);
        client_stream->bridge(server_stream.get());
        streams.push_back(std::move(client_stream));
        streams.push_back(std::move(server_stream));
        ex->drain();   // endpoint open_stream 发起
        mux_raw->deliver_open({}, std::move(streams[streams.size() - 2]));
        ex->drain();   // client link attach + hello
        mux_s_raw->deliver_accept(std::move(streams.back()));
        ex->drain();   // 服务链 attach + 回 hello；双方 ready
        assert(ready_seen);
        assert(link->ready() && link->peer_id() == "server");
        return link;
    };

    auto link1 = bring_up();
    // request/response 全链
    bool answered = false;
    link1->request("ping", std::chrono::milliseconds(5000),
                   [&](std::error_code ec, std::string payload) {
                       assert(!ec);
                       assert(payload == "ping!");
                       answered = true;
                   });
    ex->drain();
    assert(answered);

    // 第二条链路：mux 复用（不重拨），各自一条流
    auto link2 = bring_up();
    assert(client->dial_calls_ == 1);                       // 同 key 只拨过一次
    assert(client_mux_raws.front()->open_calls_ == 2);      // mux 上两条流
    assert(mux_s_raw->accept_pending());                   // accept 泵重新武装

    // 链路 1 关闭（只 fin 自己的流）：mux 仍健康，第三条链照常
    link1->stop();
    ex->drain();
    auto link3 = bring_up();
    assert(client->dial_calls_ == 1 && client_mux_raws.front()->open_calls_ == 3);

    bool answered3 = false;
    link3->request("third", std::chrono::milliseconds(5000),
                   [&](std::error_code ec, std::string payload) {
                       assert(!ec && payload == "third!");
                       answered3 = true;
                   });
    ex->drain();
    assert(answered3);

    // 收尾：listener stop 取消挂起 accept；各链停止
    listener->stop();
    link2->stop();
    link3->stop();
    for (auto& l : server_links) l->stop();
    ex->drain();
    assert(server->stopped_);
}

int main() {
    test_adapter_semantics();
    test_node_link_over_quic();
    return 0;
}
