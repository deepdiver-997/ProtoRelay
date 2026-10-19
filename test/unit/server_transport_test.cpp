// The full server/metrics facade must work with a user-supplied backend that
// exposes no Asio context, work guard, native socket or TLS context.
#undef NDEBUG
#include "framework/tcp_server_base.h"
#include "mail_system/back/router/i_shard_router.h"
#include "manual_executor.h"
#include <cassert>
#include <cstring>

#if defined(BOOST_ASIO_VERSION) || defined(BOOST_VERSION) || defined(SPDLOG_VER_MAJOR) || defined(FMT_VERSION)
#error "A network backend leaked into the server interface"
#endif

namespace {
using namespace pr;
using mail_system::test::ManualExecutor;
struct Wire {
    bool closed = false, tls = false;
    uint16_t port = 143;
    std::deque<std::string> incoming;
    std::string outgoing;
};
class TestConnection final : public IConnection {
public:
    TestConnection(std::shared_ptr<ManualExecutor> executor, std::shared_ptr<Wire> wire)
        : owner(std::move(executor)), wire(std::move(wire)) {}
    ~TestConnection() override { close(); }
    void async_read(net::MutableBuffer buffer, ReadHandler handler) override {
        auto state = wire;
        owner->post([state, buffer, handler = std::move(handler)] {
            if (state->incoming.empty()) { handler(net::make_error_code(net::IoErrc::end_of_stream), 0); return; }
            auto& data = state->incoming.front();
            const auto size = std::min(buffer.size(), data.size());
            std::memcpy(buffer.data(), data.data(), size);
            data.erase(0, size);
            if (data.empty()) state->incoming.pop_front();
            handler({}, size);
        });
    }
    void async_write(net::ConstBuffer buffer, WriteHandler handler) override {
        auto state = wire;
        owner->post([state, buffer, handler = std::move(handler)] {
            state->outgoing.append(static_cast<const char*>(buffer.data()), buffer.size());
            handler({}, buffer.size());
        });
    }
    void async_handshake(net::HandshakeRole, HandshakeHandler handler) override {
        owner->post([handler = std::move(handler)] { handler({}); });
    }
    std::shared_ptr<net::IExecutor> executor() override { return owner; }
    void close() override { if (wire) wire->closed = true; }
    bool is_open() const override { return wire && !wire->closed; }
    uint16_t get_local_port() const override { return wire->port; }
    std::string get_remote_ip() const override { return "192.0.2.1"; }
    std::shared_ptr<ManualExecutor> owner;
    std::shared_ptr<Wire> wire;
};
struct ListenerState {
    net::IServerTransport::AcceptHandler handler;
    std::vector<ListenerConfig> listeners;
    bool stopped = false, fail_tls = false;
};
class TestTransport final : public net::IServerTransport {
public:
    explicit TestTransport(std::shared_ptr<ListenerState> state) : state(std::move(state)) {}
    ~TestTransport() override { stop(); }
    const std::vector<ListenerConfig>& listeners() const override { return state->listeners; }
    void start(AcceptHandler handler) override { state->handler = std::move(handler); }
    void stop() override { state->stopped = true; state->handler = {}; }
    void configure_tls(const net::TlsConfig&) override {}
    std::unique_ptr<IConnection> upgrade_tls(std::unique_ptr<IConnection> connection) override {
        if (state->fail_tls) throw std::system_error(std::make_error_code(std::errc::operation_not_supported));
        auto& plain = dynamic_cast<TestConnection&>(*connection);
        auto wire = std::move(plain.wire);
        wire->tls = true;
        return std::make_unique<TestConnection>(plain.owner, std::move(wire));
    }
    std::shared_ptr<ListenerState> state;
};
class Runtime final : public ThreadPoolBase, public net::IExecutorProvider, public net::IServerTransportProvider {
public:
    void start() override {}
    void stop(bool = true) override {}
    size_t thread_count() const override { return 1; }
    bool is_running() const override { return true; }
    std::shared_ptr<net::IExecutor> next_executor() override { return executor; }
    std::unique_ptr<net::IServerTransport> make_server_transport(const net::ServerTransportConfig& config) override {
        last = std::make_shared<ListenerState>();
        last->listeners = config.listeners;
        for (auto& listener : last->listeners) if (listener.port == 0) listener.port = 28143;
        return std::make_unique<TestTransport>(last);
    }
    void accept(std::shared_ptr<Wire> wire) {
        auto state = last;
        auto owner = executor;
        owner->post([state, owner, wire] {
            if (!state->stopped) state->handler(std::make_unique<TestConnection>(owner, wire), state->listeners.front());
        });
    }
    std::shared_ptr<ManualExecutor> executor = std::make_shared<ManualExecutor>();
    std::shared_ptr<ListenerState> last;
protected:
    void post_impl(std::function<void()> task) override { executor->post(std::move(task)); }
};
class Router final : public mail_system::router::IShardRouter {
public:
    int route(const std::string&) override { return -1; }
    size_t shard_count() const override { return 0; }
    const char* name() const override { return "test"; }
    std::shared_ptr<DBPool> get_db_pool(size_t) override { return {}; }
    std::shared_ptr<storage::IStorageProvider> get_storage(size_t) override { return {}; }
};
template<class Connection> struct Session {
    std::unique_ptr<Connection> connection;
    bool started = false, upgraded = false, tracks = false;
    std::string trace;
    void set_tracks_connection_count(bool value) { tracks = value; }
    void set_trace_buffer(std::string value) { trace = std::move(value); }
    static void start(std::shared_ptr<Session> self) { self->started = true; }
    static void start_after_starttls(std::shared_ptr<Session> self) { self->upgraded = true; }
};
class Server final : public TcpServerBase<Session<TcpConnection>, Session<SslConnection>> {
public:
    using TcpServerBase::TcpServerBase;
    using TcpServerBase::stop;
    ~Server() override { stop(); }
    std::shared_ptr<Session<TcpConnection>> tcp;
    std::shared_ptr<Session<SslConnection>> tls;
    ListenerConfig received;
    bool reject = false;
protected:
    bool should_reject_connection(std::string&, const std::string&) const override { return reject; }
    std::shared_ptr<Session<TcpConnection>> make_tcp_session(std::unique_ptr<TcpConnection> c, const ListenerConfig& lc) override {
        received = lc; tcp = std::make_shared<Session<TcpConnection>>(); tcp->connection = std::move(c); return tcp;
    }
    std::shared_ptr<Session<SslConnection>> make_ssl_session(std::unique_ptr<SslConnection> c, const ListenerConfig& lc) override {
        received = lc; tls = std::make_shared<Session<SslConnection>>(); tls->connection = std::move(c); return tls;
    }
};
void server_contract() {
    auto runtime = std::make_shared<Runtime>();
    auto router = std::make_shared<Router>();
    mail_system::ServerConfig config;
    config.address = "127.0.0.1"; config.metrics_enabled = false; config.intrusion_detection_enabled = false;
    ListenerConfig listener(ListenerType::TCP, 143);
    listener.auth_policy = InboundAuthPolicy::ON; listener.advertise_starttls = false;
    config.listeners = {listener};
    auto server = std::make_unique<Server>(config, runtime, runtime, router);
    server->start();
    auto wire = std::make_shared<Wire>();
    runtime->accept(wire); runtime->executor->drain();
    assert(server->tcp->started && server->tcp->tracks && server->active_connections_ == 1);
    assert(server->received.auth_policy == InboundAuthPolicy::ON && !server->received.advertise_starttls);
    auto executor = server->tcp->connection->executor();
    executor->post([&] { server->handoff_starttls(std::move(server->tcp->connection), "pre-TLS trace"); });
    runtime->executor->drain();
    assert(wire->tls && !wire->closed && server->tls->upgraded && server->tls->trace == "pre-TLS trace");
    assert(server->tls->connection->executor() == executor && server->received.auth_policy == InboundAuthPolicy::ON);
    assert(server->active_connections_ == 2);
    server->reject = true;
    auto denied = std::make_shared<Wire>();
    runtime->accept(denied); runtime->executor->drain();
    assert(denied->closed && server->connections_rejected_total_ == 1 && server->connections_total_ == 1);
    auto state = runtime->last;
    runtime->accept(std::make_shared<Wire>()); // accept queued immediately before destruction
    server.reset();
    assert(state->stopped && !state->handler);
    runtime->executor->drain(); // cannot call the destroyed server
}
void tls_accept_and_failure() {
    auto runtime = std::make_shared<Runtime>();
    auto router = std::make_shared<Router>();
    mail_system::ServerConfig config;
    config.address = "127.0.0.1"; config.metrics_enabled = false; config.intrusion_detection_enabled = false;
    config.listeners.emplace_back(ListenerType::SSL, 0);
    {
        Server server(config, runtime, runtime, router); server.start();
        assert(server.m_listener_configs.count(28143) == 1);
        auto wire = std::make_shared<Wire>(); wire->tls = true; wire->port = 28143;
        runtime->accept(wire); runtime->executor->drain();
        assert(server.tls->started && !server.tls->upgraded && !server.tcp);
        assert(server.received.type == ListenerType::SSL && server.received.port == 28143);
    }
    config.listeners.front() = ListenerConfig(ListenerType::TCP, 143);
    Server server(config, runtime, runtime, router); server.start();
    auto wire = std::make_shared<Wire>();
    runtime->accept(wire); runtime->executor->drain();
    runtime->last->fail_tls = true;
    bool failed = false;
    runtime->executor->post([&] {
        try { server.handoff_starttls(std::move(server.tcp->connection)); }
        catch (const std::system_error& error) { failed = error.code() == std::errc::operation_not_supported; }
    });
    runtime->executor->drain();
    assert(failed && wire->closed && !server.tls && server.active_connections_ == 1);
}
void metrics_contract() {
    auto runtime = std::make_shared<Runtime>();
    int reloads = 0;
    auto metrics = std::make_shared<MetricsServer>(runtime, 0, "127.0.0.1", [&] { ++reloads; return "OK"; });
    metrics->inc_counter("test_total", {}, 7);
    metrics->start(); assert(metrics->bound_port() == 28143);
    auto request = [&](std::deque<std::string> chunks) {
        auto wire = std::make_shared<Wire>(); wire->incoming = std::move(chunks);
        runtime->accept(wire); runtime->executor->drain();
        assert(wire->closed); return wire->outgoing;
    };
    auto reply = request({"GET /met", "rics HTTP/1.1\r\nHost: local\r\n", "\r\n"});
    assert(reply.find("200 OK") != std::string::npos && reply.find("test_total 7") != std::string::npos);
    assert(request({"POST /reload HTTP/1.1\r\n\r\n"}).find("200 OK") != std::string::npos && reloads == 1);
    assert(request({"GET /health/live HTTP/1.1\r\n\r\n"}).find("200 OK") != std::string::npos);
    auto pending = std::make_shared<Wire>(); pending->incoming = {"GET /metrics HTTP/1.1\r\n\r\n"};
    runtime->accept(pending);
    // Enter the accept callback and initiate a read, then destroy metrics before
    // the queued read completion. Its connection survives, its owner does not.
    for (int i = 0; i < 2; ++i) {
        auto task = std::move(runtime->executor->tasks.front());
        runtime->executor->tasks.pop_front(); task();
    }
    metrics.reset(); runtime->executor->drain();
    assert(pending->closed && pending->outgoing.empty());
}
}
int main() { server_contract(); tls_accept_and_failure(); metrics_contract(); }
