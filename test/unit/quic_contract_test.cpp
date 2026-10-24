// This translation unit exercises caller-provided implementations of the
// QUIC multiplexing contracts. quic_transport.h itself must compile with no
// backend headers at all; server_transport.h is included only to build a
// complete TlsConfig for configure_tls (the contract references it by
// reference), and no OpenSSL/lsquic/Boost header may appear here.
#undef NDEBUG
#include "framework/net/quic_transport.h"
#include "framework/net/server_transport.h" // TlsConfig definition
#include <cassert>
#include <deque>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(BOOST_ASIO_VERSION) || defined(BOOST_VERSION) || defined(SPDLOG_VER_MAJOR) \
    || defined(FMT_VERSION) || defined(OPENSSL_VERSION_NUMBER)
#error "External backend leaked into QUIC contracts"
#endif

// The contracts are interfaces: no hidden inline implementation, factories are
// declarations only (no link dependency on the optional protorelay_quic
// module).
static_assert(std::is_abstract<pr::net::IMuxConnection>::value, "");
static_assert(std::is_abstract<pr::net::IQuicServerTransport>::value, "");
static_assert(std::is_abstract<pr::net::IQuicClientTransport>::value, "");
using ServerFactory = std::unique_ptr<pr::net::IQuicServerTransport>(
    *)(const pr::net::QuicServerConfig&, std::shared_ptr<pr::net::IExecutor>);
using ClientFactory = std::shared_ptr<pr::net::IQuicClientTransport>(*)(
    std::shared_ptr<pr::net::IExecutor>);
static_assert(std::is_convertible<decltype(&pr::net::quic::make_quic_server_transport),
                                  ServerFactory>::value, "");
static_assert(std::is_convertible<decltype(&pr::net::quic::make_quic_client_transport),
                                  ClientFactory>::value, "");

class ManualExecutor final : public pr::net::IExecutor {
public:
    void post(Task task) override { tasks.push_back(std::move(task)); }
    std::shared_ptr<pr::net::ITimer> make_timer() override { return {}; }
    void drain() {
        while (!tasks.empty()) { auto task = std::move(tasks.front()); tasks.pop_front(); task(); }
    }
    std::deque<Task> tasks;
};

class MockStream final : public pr::IConnection {
public:
    explicit MockStream(std::shared_ptr<ManualExecutor> executor) : executor_(std::move(executor)) {}
    std::shared_ptr<pr::net::IExecutor> executor() override { return executor_; }
    void async_read(pr::net::MutableBuffer, ReadHandler handler) override {
        executor_->post([handler = std::move(handler)] {
            handler(pr::net::make_error_code(pr::net::IoErrc::end_of_stream), 0);
        });
    }
    void async_write(pr::net::ConstBuffer buffer, WriteHandler handler) override {
        executor_->post([buffer, handler = std::move(handler)] { handler({}, buffer.size()); });
    }
    void async_handshake(pr::net::HandshakeRole, HandshakeHandler handler) override {
        executor_->post([handler = std::move(handler)] { handler({}); });
    }
    void close() override { open_ = false; }
    bool is_open() const override { return open_; }
    uint16_t get_local_port() const override { return 0; }
    std::string get_remote_ip() const override { return "127.0.0.1"; }
private:
    std::shared_ptr<ManualExecutor> executor_;
    bool open_ = true;
};

// Records initiation; the test drives completion through the deliver_*
// helpers. Models the contract discipline: completions are posted to the
// executor (never inline), exactly once per initiation; initiation after the
// connection ended still completes, with operation_canceled.
class MockMuxConnection final : public pr::net::IMuxConnection {
public:
    explicit MockMuxConnection(std::shared_ptr<ManualExecutor> executor)
        : executor_(std::move(executor)) {}
    std::shared_ptr<pr::net::IExecutor> executor() const override { return executor_; }
    bool is_open() const override { return open_; }
    pr::net::MuxCloseInfo close_reason() const override { return close_reason_; }

    void open_stream(StreamHandler handler) override {
        assert(!open_pending_);
        if (!open_) { post_empty(std::move(handler), canceled()); return; }
        open_pending_ = true;
        open_handler_ = std::move(handler);
    }
    void accept_stream(StreamHandler handler) override {
        assert(!accept_pending_);
        if (!open_) { post_empty(std::move(handler), canceled()); return; }
        accept_pending_ = true;
        accept_handler_ = std::move(handler);
    }
    void cancel() override {
        // Cancel is not close: a displaced accept keeps the connection open.
        displace(open_pending_, open_handler_, canceled());
        displace(accept_pending_, accept_handler_, canceled());
    }
    void close() override {
        if (!open_) return;
        open_ = false;
        close_reason_ = pr::net::MuxCloseInfo{std::error_code{}, "local close"};
        displace(open_pending_, open_handler_, canceled());
        displace(accept_pending_, accept_handler_, canceled());
    }

    // Test drivers — completions are posted, never inline. unique_ptr
    // payloads ride through the copyable Task in a shared_ptr box.
    void deliver_stream(std::unique_ptr<pr::IConnection> stream, bool peer_initiated) {
        bool& pending = peer_initiated ? accept_pending_ : open_pending_;
        StreamHandler& handler = peer_initiated ? accept_handler_ : open_handler_;
        assert(pending);
        pending = false;
        auto box = std::make_shared<std::unique_ptr<pr::IConnection>>(std::move(stream));
        executor_->post([handler, box]() mutable { handler({}, std::move(*box)); });
    }
    void deliver_peer_end() {
        open_ = false;
        close_reason_ = pr::net::MuxCloseInfo{pr::net::make_error_code(pr::net::IoErrc::end_of_stream),
                                              "peer closed"};
        displace(open_pending_, open_handler_, pr::net::make_error_code(pr::net::IoErrc::end_of_stream));
        displace(accept_pending_, accept_handler_, pr::net::make_error_code(pr::net::IoErrc::end_of_stream));
    }

private:
    static std::error_code canceled() { return std::make_error_code(std::errc::operation_canceled); }
    void post_empty(StreamHandler handler, std::error_code error) {
        executor_->post([handler = std::move(handler), error]() mutable { handler(error, nullptr); });
    }
    void displace(bool& pending, StreamHandler& handler, std::error_code error) {
        if (!pending) return;
        pending = false;
        post_empty(std::move(handler), std::move(error));
    }

    std::shared_ptr<ManualExecutor> executor_;
    StreamHandler open_handler_, accept_handler_;
    bool open_pending_ = false, accept_pending_ = false;
    bool open_ = true;
    pr::net::MuxCloseInfo close_reason_;
};

class MockServerTransport final : public pr::net::IQuicServerTransport {
public:
    MockServerTransport(pr::net::QuicServerConfig config,
                        std::shared_ptr<ManualExecutor> executor)
        : config_(std::move(config)), executor_(std::move(executor)) {
        if (config_.port == 0) config_.port = 20201; // port 0 replaced on bind
    }
    const pr::net::QuicServerConfig& config() const override { return config_; }
    void configure_tls(const pr::net::TlsConfig& tls) override {
        assert(!started_);
        tls_configured_ = true;
        certificate_ = tls.certificate_file;
    }
    void start(MuxAcceptHandler handler) override {
        assert(!started_ && tls_configured_);
        started_ = true;
        accept_handler_ = std::move(handler);
    }
    void stop() override {
        stopped_ = true;
        accept_handler_ = nullptr;
    }
    // Test driver: delivers one established connection through the executor.
    void deliver(std::unique_ptr<pr::net::IMuxConnection> connection) {
        assert(started_ && !stopped_);
        auto box = std::make_shared<std::unique_ptr<pr::net::IMuxConnection>>(std::move(connection));
        executor_->post([handler = accept_handler_, box]() mutable { handler(std::move(*box)); });
    }
private:
    pr::net::QuicServerConfig config_;
    std::shared_ptr<ManualExecutor> executor_;
    MuxAcceptHandler accept_handler_;
    std::string certificate_;
    bool started_ = false, stopped_ = false, tls_configured_ = false;
};

class MockClientTransport final : public pr::net::IQuicClientTransport {
public:
    explicit MockClientTransport(std::shared_ptr<ManualExecutor> executor)
        : executor_(std::move(executor)) {}
    std::shared_ptr<pr::net::IExecutor> executor() const override { return executor_; }
    void async_connect(const std::string&, uint16_t, const pr::net::ClientTlsConfig&,
                       ConnectHandler handler) override {
        assert(!pending_);
        pending_ = true;
        handler_ = std::move(handler);
    }
    // Test driver.
    void deliver(std::error_code error, std::unique_ptr<pr::net::IMuxConnection> connection) {
        assert(pending_);
        pending_ = false;
        auto box = std::make_shared<std::unique_ptr<pr::net::IMuxConnection>>(std::move(connection));
        executor_->post([handler = std::move(handler_), error, box]() mutable {
            handler(error, std::move(*box));
        });
    }
private:
    std::shared_ptr<ManualExecutor> executor_;
    ConnectHandler handler_;
    bool pending_ = false;
};

int main() {
    auto executor = std::make_shared<ManualExecutor>();

    // open_stream: success delivered on the executor, exactly once.
    {
        MockMuxConnection connection(executor);
        int calls = 0;
        connection.open_stream([&](const std::error_code& error,
                                   std::unique_ptr<pr::IConnection> stream) {
            assert(!error && stream && stream->is_open());
            ++calls;
        });
        assert(calls == 0); // never inline
        connection.deliver_stream(std::make_unique<MockStream>(executor), false);
        assert(calls == 0);
        executor->drain();
        assert(calls == 1);
        assert(connection.is_open());
        connection.close();
        assert(!connection.is_open());
        assert(connection.close_reason().code == std::error_code{}); // clean local close
    }

    // accept_stream: cancel() displaces the pending accept and keeps the
    // connection open (IConnection::cancel discipline).
    {
        MockMuxConnection connection(executor);
        int calls = 0;
        connection.accept_stream([&](const std::error_code& error,
                                     std::unique_ptr<pr::IConnection>) {
            assert(error == std::errc::operation_canceled);
            ++calls;
        });
        connection.cancel();
        executor->drain();
        assert(calls == 1 && connection.is_open());
    }

    // close() during a pending accept: operation_canceled, and initiation
    // after the close still completes (with the same error) instead of
    // hanging.
    {
        MockMuxConnection connection(executor);
        int calls = 0;
        connection.accept_stream([&](const std::error_code& error,
                                     std::unique_ptr<pr::IConnection>) {
            assert(error == std::errc::operation_canceled);
            ++calls;
        });
        connection.close();
        executor->drain();
        assert(calls == 1);
        connection.open_stream([&](const std::error_code& error,
                                   std::unique_ptr<pr::IConnection>) {
            assert(error == std::errc::operation_canceled);
            ++calls;
        });
        executor->drain();
        assert(calls == 2);
    }

    // Peer end while accepting: end_of_stream, and the reason is retrievable.
    {
        MockMuxConnection connection(executor);
        int calls = 0;
        connection.accept_stream([&](const std::error_code& error,
                                     std::unique_ptr<pr::IConnection>) {
            assert(error == pr::net::IoErrc::end_of_stream);
            ++calls;
        });
        connection.deliver_peer_end();
        executor->drain();
        assert(calls == 1 && !connection.is_open());
        assert(connection.close_reason().code == pr::net::IoErrc::end_of_stream);
        assert(connection.close_reason().detail == "peer closed");
    }

    // Delivered streams satisfy IConnection: a write completes with the full
    // buffer on the same executor.
    {
        MockMuxConnection connection(executor);
        std::unique_ptr<pr::IConnection> stream;
        connection.open_stream([&](const std::error_code& error,
                                   std::unique_ptr<pr::IConnection> opened) {
            assert(!error);
            stream = std::move(opened);
        });
        connection.deliver_stream(std::make_unique<MockStream>(executor), false);
        executor->drain();
        assert(stream);
        std::vector<char> data{'h', 'i'};
        int writes = 0;
        stream->async_write(pr::net::buffer(data),
                            [&](const std::error_code& error, std::size_t size) {
                                assert(!error && size == data.size());
                                ++writes;
                            });
        assert(writes == 0);
        executor->drain();
        assert(writes == 1);
    }

    // Server transport: bind-time port replacement, TLS identity configured
    // before start, established-only delivery on the executor, stop() ends
    // delivery.
    {
        pr::net::QuicServerConfig config;
        config.address = "127.0.0.1";
        MockServerTransport server(config, executor);
        assert(server.config().address == "127.0.0.1" && server.config().port != 0);
        pr::net::TlsConfig tls;
        tls.certificate_file = "cert.pem";
        tls.private_key_file = "key.pem";
        server.configure_tls(tls);
        int calls = 0;
        server.start([&](std::unique_ptr<pr::net::IMuxConnection> connection) {
            assert(connection && connection->is_open());
            ++calls;
        });
        server.deliver(std::make_unique<MockMuxConnection>(executor));
        assert(calls == 0);
        executor->drain();
        assert(calls == 1);
        server.stop();
    }

    // Client transport: one dial at a time, established connection delivered.
    {
        MockClientTransport client(executor);
        int calls = 0;
        pr::net::ClientTlsConfig tls;
        tls.server_name = "localhost";
        client.async_connect("127.0.0.1", 443, tls,
                             [&](const std::error_code& error,
                                 std::unique_ptr<pr::net::IMuxConnection> connection) {
                                 assert(!error && connection && connection->is_open());
                                 ++calls;
                             });
        client.deliver({}, std::make_unique<MockMuxConnection>(executor));
        assert(calls == 0);
        executor->drain();
        assert(calls == 1);
    }
    return 0;
}
