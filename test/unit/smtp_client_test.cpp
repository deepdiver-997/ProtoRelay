// 共享 SMTP 内核：真实分片 TCP/TLS 对端验证，不依赖数据库或外网。
#undef NDEBUG
#include <cassert>
#include <iostream>
#include "smtp_test_peer.h"
#include "mail_system/back/smtp_client/client_session.h"
#include "framework/net/asio_client_transport.h"
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <thread>
using namespace mail_system::smtp;
using namespace smtp_test;

std::vector<Step> transaction(std::string final_reply = "250 Stored\r\n") {
    return {{"MAIL FROM:<sender@test.local>\r\n", "250 OK\r\n"},
            {"RCPT TO:<a@test.local>\r\n", "250 OK\r\n"},
            {"RCPT TO:<b@test.local>\r\n", "251 Forward\r\n"},
            {"DATA\r\n", "354 Send\r\n"},
            {"Subject: test\r\n\r\n..dot\r\n.\r\n", std::move(final_reply), true}};
}
Message message() {
    return {"sender@test.local", {"a@test.local", "b@test.local"},
            std::make_shared<const std::string>("Subject: test\r\n\r\n.dot\r\n"), std::chrono::seconds(2)};
}
ConnectOptions options(uint16_t port) {
    ConnectOptions opts;
    opts.host = "127.0.0.1"; opts.port = port; opts.helo = "client.test";
    opts.timeout = std::chrono::seconds(2);
    return opts;
}
void test_reuse() {
    asio::io_context io;
    std::vector<Step> steps{{"EHLO client.test\r\n", "250-test.local\r\n250 SIZE 10000\r\n"}};
    for (int i = 0; i < 2; ++i) {
        auto tx = transaction(); steps.insert(steps.end(), tx.begin(), tx.end());
    }
    steps.push_back({"QUIT\r\n", "221\r\n"});
    auto peer = std::make_shared<Peer>(io, steps); peer->start();
    auto client = std::make_shared<ClientSession>(pr::net::make_asio_client_transport(std::make_shared<pr::net::AsioExecutor>(io.get_executor())));
    int accepted = 0;
    client->async_connect(options(peer->port()), [&, client](Result r) {
        assert(r.outcome == Outcome::Ready && r.reusable);
        client->async_send(message(), [&, client](Result r) {
            assert(r.outcome == Outcome::Accepted && r.recipients.size() == 2);
            assert(r.recipients[1].code == 251); ++accepted;
            client->async_send(message(), [&, client](Result r) {
                assert(r.outcome == Outcome::Accepted && r.reusable); ++accepted;
                client->async_quit([&](Result r) { assert(r.outcome == Outcome::Ready); peer->stop(); });
            });
        });
    });
    std::thread other([&] { io.run(); }); io.run(); other.join();
    assert(accepted == 2 && peer->completed == steps.size() && !client->is_open());
}
void test_failure(std::vector<Step> tx, Outcome expected, std::string stage, int code) {
    asio::io_context io;
    tx.insert(tx.begin(), {"EHLO client.test\r\n", "250 Ready\r\n"});
    auto peer = std::make_shared<Peer>(io, tx); peer->start();
    ClientSession client(pr::net::make_asio_client_transport(std::make_shared<pr::net::AsioExecutor>(io.get_executor())));
    int calls = 0;
    client.async_connect(options(peer->port()), [&](Result r) {
        assert(r.outcome == Outcome::Ready);
        client.async_send(message(), [&](Result r) {
            ++calls; assert(r.outcome == expected && r.stage == stage && r.code == code);
            if (stage == "recipient") assert(r.recipients.size() == 2 && r.recipients.back().code == code);
            peer->stop();
        });
    });
    io.run();
    assert(calls == 1 && !client.is_open() && peer->completed == tx.size());
}
struct TlsFixture {
    std::shared_ptr<asio::ssl::context> server = std::make_shared<asio::ssl::context>(asio::ssl::context::tls_server);
    std::string ca_pem;
    TlsFixture() {
        // 测试证书/私钥只在进程内生成；SAN localhost，客户端显式信任该 CA。
        auto* generator = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        assert(generator && EVP_PKEY_keygen_init(generator) == 1);
        assert(EVP_PKEY_CTX_set_rsa_keygen_bits(generator, 2048) == 1);
        EVP_PKEY* key = nullptr; assert(EVP_PKEY_keygen(generator, &key) == 1);
        EVP_PKEY_CTX_free(generator);
        X509* cert = X509_new(); assert(cert);
        X509_set_version(cert, 2); ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
        X509_gmtime_adj(X509_getm_notBefore(cert), -60); X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
        assert(X509_set_pubkey(cert, key) == 1);
        auto* name = X509_get_subject_name(cert);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
        X509_set_issuer_name(cert, name);
        auto* san = X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name, const_cast<char*>("DNS:localhost"));
        assert(san && X509_add_ext(cert, san, -1) == 1); X509_EXTENSION_free(san);
        assert(X509_sign(cert, key, EVP_sha256()) > 0);
        assert(SSL_CTX_use_certificate(server->native_handle(), cert) == 1);
        assert(SSL_CTX_use_PrivateKey(server->native_handle(), key) == 1);
        BIO* pem = BIO_new(BIO_s_mem()); assert(pem);
        assert(PEM_write_bio_X509(pem, cert) == 1);
        char* bytes = nullptr; const auto size = BIO_get_mem_data(pem, &bytes);
        ca_pem.assign(bytes, static_cast<size_t>(size)); BIO_free(pem);
        X509_free(cert); EVP_PKEY_free(key);
    }
};
void test_tls(TlsFixture& fixture, TlsMode mode, bool trusted, bool hostname) {
    asio::io_context io;
    std::vector<Step> steps;
    if (mode == TlsMode::StartTlsRequired) {
        steps.push_back({"EHLO client.test\r\n", "250-test.local\r\n250 STARTTLS\r\n"});
        steps.push_back({"STARTTLS\r\n", "220 Upgrade\r\n", false, false, true});
    }
    steps.push_back({"EHLO client.test\r\n", "250-test.local\r\n250 AUTH PLAIN\r\n"});
    steps.push_back({"AUTH PLAIN\r\n", "334\r\n"});
    steps.push_back({"AHVzZXIAc2VjcmV0\r\n", "235 Welcome\r\n"});
    auto tx = transaction(); steps.insert(steps.end(), tx.begin(), tx.end());
    // QUIT 断连不应撤回已送达结果。
    steps.push_back({"QUIT\r\n", "", false, true});
    auto peer = std::make_shared<Peer>(io, steps, "220 TLS test\r\n", fixture.server, mode == TlsMode::Implicit);
    peer->start(); ClientSession client(pr::net::make_asio_client_transport(std::make_shared<pr::net::AsioExecutor>(io.get_executor())));
    auto opts = options(peer->port()); opts.tls = mode;
    opts.tls_server_name = hostname ? "localhost" : "wrong.test";
    opts.tls_config.use_system_ca = false;
    if (trusted) opts.tls_config.ca_pem = fixture.ca_pem;
    opts.auth_user = "user"; opts.password = std::make_shared<Secret>("secret");
    int calls = 0;
    client.async_connect(std::move(opts), [&](Result r) {
        if (!trusted || !hostname) {
            assert(r.outcome == Outcome::TemporaryFailure && r.stage == "tls"); ++calls; peer->stop(); return;
        }
        assert(r.outcome == Outcome::Ready);
        client.async_send(message(), [&](Result r) {
            assert(r.outcome == Outcome::Accepted); ++calls;
            client.async_quit([&](Result r) { assert(r.outcome == Outcome::TemporaryFailure); peer->stop(); });
        });
    });
    io.run();
    assert(calls == 1 && !client.is_open());
    if (trusted && hostname) assert(peer->completed == steps.size());
    else assert(peer->completed <= 2); // 证书失败前不会发送认证凭据或邮件
}
void test_starttls_required() {
    asio::io_context io;
    auto peer = std::make_shared<Peer>(io, std::vector<Step>{
        {"EHLO client.test\r\n", "250-test.local\r\n250 AUTH PLAIN\r\n"}});
    peer->start(); ClientSession client(pr::net::make_asio_client_transport(std::make_shared<pr::net::AsioExecutor>(io.get_executor())));
    auto opts = options(peer->port()); opts.tls = TlsMode::StartTlsRequired;
    opts.auth_user = "user"; opts.password = std::make_shared<Secret>("secret");
    int calls = 0;
    client.async_connect(std::move(opts), [&](Result r) {
        assert(r.outcome == Outcome::ProtocolError && r.stage == "ehlo"); ++calls; peer->stop();
    });
    io.run(); assert(calls == 1 && peer->completed == 1);
}

void test_cancel_reconnect() {
    asio::io_context io;
    auto hang = std::make_shared<Peer>(io, std::vector<Step>{}, ""); hang->start();
    auto ready = std::make_shared<Peer>(io, std::vector<Step>{{"EHLO client.test\r\n", "250 OK\r\n"}}); ready->start();
    ClientSession client(pr::net::make_asio_client_transport(std::make_shared<pr::net::AsioExecutor>(io.get_executor())));
    int cancelled = 0, busy = 0, connected = 0;
    client.async_connect(options(hang->port()), [&](Result r) {
        assert(r.outcome == Outcome::Cancelled); ++cancelled; hang->stop();
        client.async_connect(options(ready->port()), [&](Result r) {
            assert(r.outcome == Outcome::Ready); ++connected; ready->stop(); client.close();
        });
    });
    client.async_send(message(), [&](Result r) { assert(r.stage == "busy"); ++busy; });
    asio::steady_timer timer(io, std::chrono::milliseconds(20));
    timer.async_wait([&](auto ec) { assert(!ec); client.close(); });
    io.run(); assert(cancelled == 1 && busy == 1 && connected == 1);
}
int main() {
    test_reuse();
    auto tx = transaction(); tx[2].reply = "550 Missing\r\n"; tx.resize(3);
    test_failure(tx, Outcome::Rejected, "recipient", 550);
    tx[2].reply = "450 Later\r\n";
    test_failure(tx, Outcome::TemporaryFailure, "recipient", 450);
    tx = transaction("451 Storage failed\r\n");
    test_failure(tx, Outcome::TemporaryFailure, "body", 451);
    tx.back().disconnect = true;
    test_failure(tx, Outcome::OutcomeUnknown, "body", 0);
    tx = transaction(); tx.front().disconnect = true; tx.resize(1);
    test_failure(tx, Outcome::TemporaryFailure, "mail", 0);
    test_cancel_reconnect();
    test_starttls_required();
    TlsFixture fixture;
    for (auto mode : {TlsMode::Implicit, TlsMode::StartTlsRequired}) {
        test_tls(fixture, mode, true, true);
        test_tls(fixture, mode, false, true);
        test_tls(fixture, mode, true, false);
    }
    std::cout << "SMTP client: reuse, fragmented replies, RCPT policy, outcomes, cancellation, TLS/AUTH passed\n";
}
