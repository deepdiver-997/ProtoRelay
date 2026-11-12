// Native backend through the FRAMEWORK, not just through its own transport
// classes: a real TcpServerBase-derived server with SessionBase sessions, TCP
// + SSL listeners, STARTTLS handoff, message pipelining, concurrent clients
// and a MetricsServer all run on NativeRuntime. This is the composition the
// io_backend=native production path uses; the transport-level tests in
// native_transport_test/native_tls_test deliberately bypass it.
//
// Everything binds port 0, so the test never depends on a fixed port and
// cannot collide with a parallel worktree.
#undef NDEBUG
#include "framework/tcp_server_base.h"
#include "framework/session_base.h"
#include "framework/metrics_server.h"
#include "framework/net/native_runtime.h"
#include "framework/net/native_tls.h"
#include "framework/net/client_transport.h"
#include "framework/thread_pool/worker_thread_pool.h"

#include <cassert>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

using namespace pr;
using namespace pr::net;
using namespace pr::net::native;
using Clock = std::chrono::steady_clock;

template <typename T>
T ready(std::future<T>& future, std::chrono::milliseconds budget = std::chrono::seconds(10)) {
    assert(future.wait_for(budget) == std::future_status::ready);
    return future.get();
}

void wait_until(const std::function<bool()>& condition,
                std::chrono::milliseconds budget = std::chrono::seconds(10)) {
    const auto deadline = Clock::now() + budget;
    while (!condition() && Clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    assert(condition());
}

// ---- process-exclusive temp directory + self-signed certificate ----

struct TmpDir {
    std::string path;
    TmpDir() {
        const char* configured = std::getenv("AGENT_TMP_ROOT");
        const std::string root =
            configured && *configured ? std::string(configured) : std::string("/tmp");
        std::filesystem::create_directories(root);
        std::string pattern = root + "/native_framework_test_XXXXXX";
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        assert(::mkdtemp(buffer.data()) != nullptr);
        path = buffer.data();
    }
    ~TmpDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    std::string file(const char* name) const { return path + "/" + name; }
};

struct PemPair {
    std::string cert;
    std::string key;
};

PemPair make_self_signed(const char* common_name, const char* sans) {
    EVP_PKEY* pkey = EVP_RSA_gen(2048);
    assert(pkey != nullptr);
    X509* x509 = X509_new();
    assert(x509 != nullptr);
    assert(X509_set_version(x509, 2) == 1);
    assert(ASN1_INTEGER_set(X509_get_serialNumber(x509), static_cast<long>(::getpid())) == 1);
    assert(X509_gmtime_adj(X509_getm_notBefore(x509), -3600) != nullptr);
    assert(X509_gmtime_adj(X509_getm_notAfter(x509), 86400) != nullptr);
    assert(X509_set_pubkey(x509, pkey) == 1);
    X509_NAME* name = const_cast<X509_NAME*>(X509_get_subject_name(x509));
    assert(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                      reinterpret_cast<const unsigned char*>(common_name), -1, -1,
                                      0) == 1);
    assert(X509_set_issuer_name(x509, name) == 1);
    if (sans && *sans) {
        X509_EXTENSION* extension = X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name, sans);
        assert(extension != nullptr);
        assert(X509_add_ext(x509, extension, -1) == 1);
        X509_EXTENSION_free(extension);
    }
    assert(X509_sign(x509, pkey, EVP_sha256()) != 0);

    auto to_pem = [](auto write) {
        BIO* bio = BIO_new(BIO_s_mem());
        assert(bio != nullptr);
        assert(write(bio) == 1);
        char* data = nullptr;
        const long size = BIO_get_mem_data(bio, &data);
        std::string pem(data, static_cast<std::size_t>(size));
        BIO_free(bio);
        return pem;
    };
    PemPair pair;
    pair.cert = to_pem([&](BIO* bio) { return PEM_write_bio_X509(bio, x509); });
    pair.key = to_pem([&](BIO* bio) {
        return PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    });
    X509_free(x509);
    EVP_PKEY_free(pkey);
    return pair;
}

void write_file(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    assert(out.write(content.data(), static_cast<std::streamsize>(content.size())));
}

// ---- raw BSD-socket helpers (blocking, bounded by SO_RCVTIMEO) ----

int connect_to(uint16_t port) {
    // BSD connect semantics: an interrupted BLOCKING connect keeps running
    // asynchronously — the socket can never be reused, so EINTR forces a
    // fresh socket rather than a retry on the same descriptor.
    for (int attempt = 0; attempt < 3; ++attempt) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        assert(fd >= 0);
        timeval timeout{5, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) return fd;
        const int error = errno;
        ::close(fd);
        if (error != EINTR) {
            std::fprintf(stderr, "connect(127.0.0.1:%u) failed: %s (errno=%d)\n", port,
                         std::strerror(error), error);
            assert(false);
        }
    }
    assert(false && "connect interrupted three times");
    return -1;
}

void send_all(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t wrote = ::send(fd, data.data() + sent, data.size() - sent, 0);
        assert(wrote > 0);
        sent += static_cast<std::size_t>(wrote);
    }
}

std::string recv_until(int fd, std::size_t total) {
    std::string out;
    char buffer[4096];
    while (out.size() < total) {
        const ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
        assert(got > 0);
        out.append(buffer, static_cast<std::size_t>(got));
    }
    return out;
}

std::string recv_to_eof(int fd) {
    std::string out;
    char buffer[4096];
    for (;;) {
        const ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
        if (got <= 0) return out;
        out.append(buffer, static_cast<std::size_t>(got));
    }
}

// ---- native client transport helpers ----

std::error_code connect_to(IClientTransport& client, const std::string& host, uint16_t port) {
    std::promise<std::error_code> promise;
    auto future = promise.get_future();
    client.async_connect(host, port,
                         [&](const std::error_code& error) { promise.set_value(error); });
    return ready(future);
}

void client_write(IClientTransport& client, const std::string& data) {
    std::promise<std::error_code> promise;
    auto future = promise.get_future();
    client.async_write(buffer(data), [&](const std::error_code& error, std::size_t bytes) {
        assert(bytes == data.size());
        promise.set_value(error);
    });
    assert(!ready(future));
}

// Buffers one line across arbitrary read boundaries. Native reads return what
// is available, so a line may arrive in several completions.
struct LineReader {
    std::string buffered;
    std::string next(IClientTransport& client) {
        while (buffered.find('\n') == std::string::npos) {
            std::vector<char> chunk(1024);
            std::promise<std::error_code> promise;
            auto future = promise.get_future();
            client.async_read(buffer(chunk), [&](const std::error_code& error, std::size_t bytes) {
                assert(!error);
                buffered.append(chunk.data(), bytes);
                promise.set_value(error);
            });
            assert(!ready(future));
        }
        const auto end = buffered.find('\n');
        std::string line = buffered.substr(0, end + 1);
        buffered.erase(0, end + 1);
        return line;
    }
};

// ---- framework session: line echo + STARTTLS handoff ----

class TestServer;

template <typename ConnectionType>
class EchoSession final : public SessionBase<ConnectionType> {
public:
    using Base = SessionBase<ConnectionType>;

    EchoSession(std::unique_ptr<ConnectionType> connection, ServerBase* server)
        : Base(std::move(connection), server) {}

    static void start(std::shared_ptr<EchoSession> self) {
        Base::do_handshake(self, HandshakeRole::server,
                           [](std::shared_ptr<Base> session, const std::error_code& error) {
                               if (!error) session->do_async_read();
                           });
    }

    static void start_after_starttls(std::shared_ptr<EchoSession> self) {
        start(std::move(self));
    }

    void handle_read(const std::string& line) override;

    void process_read() override {}

    std::chrono::milliseconds compute_reply_delay() const override { return {}; }
    bool has_buffered_input() const override {
        return this->command_read_buffer_.find('\n') != std::string::npos;
    }
    std::string extract_one_line() override {
        const auto end = this->command_read_buffer_.find('\n');
        if (end == std::string::npos) return {};
        std::string line = this->command_read_buffer_.substr(0, end + 1);
        this->command_read_buffer_.erase(0, end + 1);
        return line;
    }
    void set_current_state(int) override {}
    void set_next_event(int) override {}
    int get_current_state() const override { return 0; }
    int get_next_event() const override { return 0; }
    void* get_fsm() const override { return nullptr; }
    void* get_context() override { return nullptr; }
    std::string get_last_command_args() const override { return {}; }
};

class TestServer final
    : public TcpServerBase<EchoSession<TcpConnection>, EchoSession<SslConnection>> {
public:
    using Base = TcpServerBase<EchoSession<TcpConnection>, EchoSession<SslConnection>>;
    using Base::Base;
    using Base::stop; // the TcpServerBase stop hook is protected

    uint16_t port_of(ListenerType type) const {
        for (const auto& [port, listener] : m_listener_configs)
            if (listener.type == type) return port;
        assert(false);
        return 0;
    }

protected:
    bool should_reject_connection(std::string&, const std::string&) const override { return false; }
    std::shared_ptr<EchoSession<TcpConnection>> make_tcp_session(
        std::unique_ptr<TcpConnection> connection, const ListenerConfig&) override {
        return std::make_shared<EchoSession<TcpConnection>>(std::move(connection), this);
    }
    std::shared_ptr<EchoSession<SslConnection>> make_ssl_session(
        std::unique_ptr<SslConnection> connection, const ListenerConfig&) override {
        return std::make_shared<EchoSession<SslConnection>>(std::move(connection), this);
    }
};

template <typename ConnectionType>
void EchoSession<ConnectionType>::handle_read(const std::string& line) {
    if constexpr (std::is_same<ConnectionType, TcpConnection>::value) {
        if (line == "STARTTLS\n") {
            // Only the plain session hands its transport over; an SSL session
            // can never see this branch.
            this->do_async_write(
                "220 2.0.0 Ready to start TLS\r\n",
                [](std::shared_ptr<Base> session, const std::error_code& error) {
                    if (error) return;
                    auto* server = static_cast<TestServer*>(session->get_server());
                    server->handoff_starttls(session->release_connection());
                });
            return;
        }
    }
    this->do_async_write("echo:" + line);
}

// ---- scenarios ----

// A real server+session stack on the native runtime: TCP echo through the
// framework session, including split lines and pipelined commands.
void tcp_sessions(TestServer& server, NativeRuntime& runtime) {
    const uint16_t port = server.port_of(ListenerType::TCP);

    auto client = runtime.make_client_transport();
    assert(!connect_to(*client, "127.0.0.1", port));
    assert(client->is_open());
    assert(client->get_remote_ip() == "127.0.0.1");
    LineReader reader;
    client_write(*client, "hello\n");
    assert(reader.next(*client) == "echo:hello\n");
    wait_until([&] { return server.active_connections_.load() >= 1; });

    // Split command: the session buffer must reassemble it across reads.
    client_write(*client, "par");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    client_write(*client, "tial\n");
    assert(reader.next(*client) == "echo:partial\n");

    // Pipelined commands are consumed in order; responses follow the same order.
    client_write(*client, "one\ntwo\n");
    assert(reader.next(*client) == "echo:one\n");
    assert(reader.next(*client) == "echo:two\n");
    client->close();
}

// An SSL listener accepted by TcpServerBase (its SslConnection path) and a
// native STARTTLS handoff from a plain session to an SSL session.
void ssl_and_starttls(TestServer& server, NativeRuntime& runtime,
                      const std::string& cert_file) {
    ClientTlsConfig tls;
    tls.server_name = "localhost";
    tls.ca_file = cert_file;
    tls.use_system_ca = false;

    auto start_tls = [&](IClientTransport& client) {
        std::promise<std::error_code> promise;
        auto future = promise.get_future();
        client.async_start_tls(tls,
                               [&](const std::error_code& error) { promise.set_value(error); });
        assert(!ready(future));
    };

    // Direct TLS listener.
    {
        auto client = runtime.make_client_transport();
        assert(!connect_to(*client, "127.0.0.1", server.port_of(ListenerType::SSL)));
        start_tls(*client);
        assert(client->native_handle() == -1); // TLS hides the raw fd
        LineReader reader;
        client_write(*client, "secure-listener\n");
        assert(reader.next(*client) == "echo:secure-listener\n");
        client->close();
    }

    // STARTTLS: the plain session hands its transport to TcpServerBase, which
    // upgrades it through the native TLS hooks and starts an SSL session.
    {
        auto client = runtime.make_client_transport();
        assert(!connect_to(*client, "127.0.0.1", server.port_of(ListenerType::TCP)));
        LineReader reader;
        client_write(*client, "STARTTLS\n");
        assert(reader.next(*client) == "220 2.0.0 Ready to start TLS\r\n");
        start_tls(*client);
        client_write(*client, "after-starttls\n");
        assert(reader.next(*client) == "echo:after-starttls\n");
        client->close();
    }
}

// Many raw clients at once across both runtime loops: every session must see
// its own bytes and close cleanly.
void concurrent_clients(TestServer& server) {
    const uint16_t port = server.port_of(ListenerType::TCP);
    constexpr int kClients = 24;
    std::atomic<int> ok{0};
    std::vector<std::thread> clients;
    clients.reserve(kClients);
    for (int index = 0; index < kClients; ++index) {
        clients.emplace_back([&, index] {
            const int fd = connect_to(port);
            const std::string message = "client-" + std::to_string(index) + "\n";
            send_all(fd, message);
            const std::string expected = "echo:" + message;
            if (recv_until(fd, expected.size()) == expected) ok.fetch_add(1);
            ::close(fd);
        });
    }
    for (auto& client : clients) client.join();
    assert(ok.load() == kClients);
    wait_until([&] { return server.active_connections_.load() == 0; });
}

// MetricsServer is the other framework consumer of IServerTransportProvider;
// serve a real Prometheus scrape over the native transport.
void metrics_on_native() {
    auto runtime = std::make_shared<pr::net::native::NativeRuntime>(2);
    runtime->start();
    {
        auto metrics = std::make_shared<MetricsServer>(runtime, 0, "127.0.0.1");
        metrics->inc_counter("native_framework_total", {}, 7);
        metrics->start();
        assert(metrics->bound_port() != 0);

        const int fd = connect_to(metrics->bound_port());
        send_all(fd, "GET /metrics HTTP/1.1\r\nHost: native\r\nConnection: close\r\n\r\n");
        const std::string response = recv_to_eof(fd);
        ::close(fd);
        assert(response.find("200 OK") != std::string::npos);
        assert(response.find("native_framework_total 7") != std::string::npos);
        metrics->stop();
    }
    runtime->stop(true);
}

} // namespace

int main() {
    assert(pr::net::native::enable_native_tls());

    // Metrics first: it needs nothing but the runtime and exercises the
    // provider path in isolation.
    metrics_on_native();

    TmpDir tmp;
    const PemPair pem = make_self_signed("localhost", "DNS:localhost,IP:127.0.0.1");
    const std::string cert_file = tmp.file("server.crt");
    const std::string key_file = tmp.file("server.key");
    write_file(cert_file, pem.cert);
    write_file(key_file, pem.key);

    auto runtime = std::make_shared<pr::net::native::NativeRuntime>(2);
    runtime->start();
    auto workers = std::make_shared<WorkerThreadPool>(2);
    workers->start();

    ServerConfig config;
    config.address = "127.0.0.1";
    config.listeners = {ListenerConfig(ListenerType::TCP, 0),
                        ListenerConfig(ListenerType::SSL, 0)};
    config.certFile = cert_file;
    config.keyFile = key_file;
    config.metrics_enabled = false;
    config.intrusion_detection_enabled = false;
    config.maxConnections = 1000;

    {
        TestServer server(config, runtime, workers);
        server.start();
        tcp_sessions(server, *runtime);
        ssl_and_starttls(server, *runtime, cert_file);
        concurrent_clients(server);

        // stop() must not accept again; it also leaves existing sessions to
        // close on their own. Closing the last peer drains the count.
        const int fd = connect_to(server.port_of(ListenerType::TCP));
        wait_until([&] { return server.active_connections_.load() == 1; });
        server.stop();
        ::close(fd);
        wait_until([&] { return server.active_connections_.load() == 0; });
    }

    runtime->stop(true);
    workers->stop(true);
    return 0;
}
