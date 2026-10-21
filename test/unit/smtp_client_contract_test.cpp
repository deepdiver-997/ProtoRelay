// The SMTP state machine runs against an injected transport with no Asio types.
#undef NDEBUG
#include "mail_system/back/smtp_client/client_session.h"
#include "mail_system/back/submission/mail_submission_service.h"
#include "manual_executor.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <deque>
#include <iostream>

#if defined(BOOST_ASIO_VERSION) || defined(BOOST_VERSION)
#error "SMTP public interfaces must not expose Boost"
#endif

namespace {
using namespace mail_system::smtp;
using mail_system::test::ManualExecutor;
struct Step { std::string expected, reply; };
class Transport final : public pr::net::IClientTransport {
public:
    std::shared_ptr<ManualExecutor> owner = std::make_shared<ManualExecutor>();
    std::deque<Step> steps;
    std::string greeting = "220 test\r\n", input;
    std::string host;
    uint16_t port = 0;
    bool open = false, tls = false;
    size_t chunk = 3;
    pr::net::ClientTlsConfig tls_config;
    pr::net::MutableBuffer pending_buffer;
    ReadHandler pending_read;
    std::shared_ptr<pr::net::IExecutor> executor() override { return owner; }
    void async_connect(const std::string& value, uint16_t number, HandshakeHandler handler) override {
        assert(owner->running);
        host = value; port = number; open = true; tls = false; input = greeting;
        owner->post([handler = std::move(handler)] { handler({}); });
    }
    void async_start_tls(pr::net::ClientTlsConfig config, HandshakeHandler handler) override {
        assert(owner->running && open && !tls && !pending_read);
        tls = true; tls_config = std::move(config);
        owner->post([handler = std::move(handler)] { handler({}); });
    }
    void async_read(pr::net::MutableBuffer buffer, ReadHandler handler) override {
        assert(owner->running && !pending_read);
        pending_buffer = buffer; pending_read = std::move(handler);
        flush();
    }
    void flush() {
        if (!pending_read || input.empty()) return;
        auto handler = std::move(pending_read); pending_read = {};
        const auto count = std::min({chunk, pending_buffer.size(), input.size()});
        std::memcpy(pending_buffer.data(), input.data(), count); input.erase(0, count);
        owner->post([handler = std::move(handler), count] { handler({}, count); });
    }
    void async_write(pr::net::ConstBuffer buffer, WriteHandler handler) override {
        assert(owner->running && !steps.empty());
        auto step = std::move(steps.front()); steps.pop_front();
        assert(step.expected == std::string(static_cast<const char*>(buffer.data()), buffer.size()));
        input += step.reply;
        owner->post([handler = std::move(handler), size = buffer.size()] { handler({}, size); });
    }
    void async_handshake(pr::net::HandshakeRole, HandshakeHandler handler) override {
        owner->post([handler = std::move(handler)] { handler({}); });
    }
    void close() override {
        assert(owner->running); open = false;
        if (pending_read) {
            auto handler = std::move(pending_read); pending_read = {};
            owner->post([handler = std::move(handler)] {
                handler(std::make_error_code(std::errc::operation_canceled), 0);
            });
        }
    }
    bool is_open() const override { return open; }
    uint16_t get_local_port() const override { return 20001; }
    std::string get_remote_ip() const override { return "127.0.0.1"; }
};
ConnectOptions options() {
    ConnectOptions out; out.host = "localhost"; out.port = 25025; out.helo = "client.test";
    return out;
}
void injected_starttls() {
    auto transport = std::make_shared<Transport>();
    transport->steps = {{"EHLO client.test\r\n", "250-test\r\n250 STARTTLS\r\n"},
                        {"STARTTLS\r\n", "220 Upgrade\r\n"},
                        {"EHLO client.test\r\n", "250 Ready\r\n"},
                        {"MAIL FROM:<from@test>\r\n", "250 OK\r\n"},
                        {"RCPT TO:<to@test>\r\n", "250 OK\r\n"},
                        {"DATA\r\n", "354 Continue\r\n"},
                        {"Subject: test\r\n\r\n..body\r\n.\r\n", "250 Stored\r\n"}};
    int connected = 0, accepted = 0;
    {
        ClientSession client(transport);
        auto config = options(); config.tls = TlsMode::StartTlsRequired;
        config.tls_server_name = "mail.test";
        config.tls_config.ca_pem = "test CA supplied to adapter";
        config.tls_config.use_system_ca = false;
        client.async_connect(config, [&](Result result) {
            assert(transport->owner->running && result.outcome == Outcome::Ready); ++connected;
            assert(transport->host == "localhost" && transport->port == 25025 && transport->tls);
            assert(transport->tls_config.server_name == "mail.test");
            assert(transport->tls_config.ca_pem == config.tls_config.ca_pem);
            assert(!transport->tls_config.use_system_ca);
            client.async_send({"from@test", {"to@test"},
                std::make_shared<const std::string>("Subject: test\r\n\r\n.body\r\n")}, [&](Result result) {
                assert(transport->owner->running && result.outcome == Outcome::Accepted);
                assert(result.reusable); ++accepted;
            });
        });
        assert(!connected);
        transport->owner->drain();
        assert(connected == 1 && accepted == 1 && transport->steps.empty());
        assert(client.is_open());
        for (const auto& timer : transport->owner->timers) assert(!timer->handler);
    }
    transport->owner->drain(); assert(!transport->open);
}
void timeout_and_late_cancel() {
    auto transport = std::make_shared<Transport>(); transport->greeting.clear();
    int calls = 0;
    {
        ClientSession client(transport);
        client.async_connect(options(), [&](Result result) {
            assert(transport->owner->running);
            assert(result.outcome == Outcome::TemporaryFailure && result.error == "deadline exceeded");
            ++calls;
        });
        transport->owner->drain(); assert(!calls && transport->pending_read);
        transport->owner->timers.back()->fire();
        transport->owner->drain(); assert(calls == 1 && !client.is_open() && !transport->pending_read);
    }
    transport->owner->drain(); assert(calls == 1);
}
} // namespace
int main() {
    injected_starttls(); timeout_and_late_cancel();
    std::cout << "SMTP client backend contracts passed\n";
}
