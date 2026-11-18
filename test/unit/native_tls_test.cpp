// Native TLS adapter contract tests (enable_native_tls installed): client and
// server handshakes (STARTTLS upgrade and SSL listeners), multi-worker
// connection/executor affinity, mandatory verification failures (untrusted
// chain, hostname mismatch, no trust source),
// post-handshake contract (partial reads, write backpressure, cancel exactly
// once), clean close (close_notify -> EOF) versus truncation (no close_notify
// -> stream_truncated). Every scenario runs on every compiled poller backend.
// Self-signed certificates are generated in-process into a process-exclusive
// temp directory (AGENT_TMP_ROOT) and cleaned up.
#undef NDEBUG
#include "framework/net/native_transport.h"
#include "framework/net/native_tls.h"
#include "framework/net/tls_error.h"

#include <cassert>
#include <cstdio>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace pr;
using namespace pr::net;
using namespace pr::net::native;

namespace {

template <typename T>
bool ready_within(std::future<T>& future, std::chrono::milliseconds budget) {
    return future.wait_for(budget) == std::future_status::ready;
}

uint16_t base_port() {
    if (const char* configured = std::getenv("PROTORELAY_TEST_PORT_BASE")) {
        const int base = std::atoi(configured);
        if (base > 0) return static_cast<uint16_t>(base + 95);
    }
    return 20295;
}

// ---- process-exclusive temp directory (AGENT_TMP_ROOT when set) ----

struct TmpDir {
    std::string path;
    TmpDir() {
        const char* configured = std::getenv("AGENT_TMP_ROOT");
        const std::string root =
            configured && *configured ? std::string(configured) : std::string("/tmp");
        std::filesystem::create_directories(root);
        std::string pattern = root + "/native_tls_test_XXXXXX";
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

void write_file(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    assert(out.write(content.data(), static_cast<std::streamsize>(content.size())));
}

// ---- in-process self-signed certificate generation ----

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
    pair.key = to_pem(
        [&](BIO* bio) { return PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr); });
    X509_free(x509);
    EVP_PKEY_free(pkey);
    return pair;
}

// ---- blocking OpenSSL test peer (runs on test-side threads) ----

class TlsPeer {
public:
    // Blocking SSL_accept against a listening socket (call via std::async).
    static TlsPeer accept_server(int listener_fd, const std::string& cert_path,
                                 const std::string& key_path) {
        const int fd = ::accept(listener_fd, nullptr, nullptr);
        assert(fd >= 0);
        SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
        assert(ctx != nullptr);
        assert(SSL_CTX_use_certificate_chain_file(ctx, cert_path.c_str()) == 1);
        assert(SSL_CTX_use_PrivateKey_file(ctx, key_path.c_str(), SSL_FILETYPE_PEM) == 1);
        TlsPeer peer(ctx, fd);
        assert(SSL_accept(peer.ssl_) == 1);
        return peer;
    }

    // Blocking SSL_connect with verification disabled (raw test client).
    static TlsPeer connect_client(int socket_fd) {
        SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
        assert(ctx != nullptr);
        TlsPeer peer(ctx, socket_fd);
        SSL_set_verify(peer.ssl_, SSL_VERIFY_NONE, nullptr);
        assert(SSL_connect(peer.ssl_) == 1);
        return peer;
    }

    TlsPeer(TlsPeer&& other) noexcept { steal(std::move(other)); }
    TlsPeer& operator=(TlsPeer&& other) noexcept {
        release();
        steal(std::move(other));
        return *this;
    }
    TlsPeer(const TlsPeer&) = delete;
    TlsPeer& operator=(const TlsPeer&) = delete;
    ~TlsPeer() { release(); }

    void write_all(const std::string& data) {
        std::size_t sent = 0;
        while (sent < data.size()) {
            const int wrote = SSL_write(ssl_, data.data() + static_cast<int>(sent),
                                        static_cast<int>(data.size() - sent));
            assert(wrote > 0);
            sent += static_cast<std::size_t>(wrote);
        }
    }

    std::string read_some() {
        char chunk[4096];
        const int got = SSL_read(ssl_, chunk, sizeof(chunk));
        if (got <= 0) return {};
        return std::string(chunk, static_cast<std::size_t>(got));
    }

    // Clean TLS close: close_notify first, then the socket.
    void clean_close() {
        if (ssl_) SSL_shutdown(ssl_);
        release();
    }

    // Truncation: socket gone without close_notify (plain SSL_free).
    void truncate_close() { release(); }

private:
    TlsPeer(SSL_CTX* ctx, int fd) : ctx_(ctx), fd_(fd) {
        ssl_ = SSL_new(ctx_);
        assert(ssl_ != nullptr);
        SSL_set_fd(ssl_, fd_);
    }
    void steal(TlsPeer&& other) {
        ctx_ = other.ctx_;
        ssl_ = other.ssl_;
        fd_ = other.fd_;
        other.ctx_ = nullptr;
        other.ssl_ = nullptr;
        other.fd_ = -1;
    }
    void release() {
        if (ssl_) SSL_free(ssl_);
        if (ctx_) SSL_CTX_free(ctx_);
        if (fd_ >= 0) ::close(fd_);
        ssl_ = nullptr;
        ctx_ = nullptr;
        fd_ = -1;
    }
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    int fd_ = -1;
};

// ---- plain BSD-socket test scaffolding (same shapes as native_transport_test) ----

int open_connected_socket(uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    return fd;
}

struct RawListener {
    int fd = -1;
    uint16_t port = 0;
    explicit RawListener(uint16_t hint) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        assert(fd >= 0);
        const int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bool bound = false;
        for (int attempt = 0; attempt < 3 && !bound; ++attempt) {
            address.sin_port = htons(attempt == 0 ? hint : 0);
            bound = ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
            assert(bound || errno == EADDRINUSE);
        }
        assert(bound);
        assert(::listen(fd, 16) == 0);
        sockaddr_in actual{};
        socklen_t length = sizeof(actual);
        assert(::getsockname(fd, reinterpret_cast<sockaddr*>(&actual), &length) == 0);
        port = ntohs(actual.sin_port);
    }
    ~RawListener() {
        if (fd >= 0) ::close(fd);
    }
    RawListener(const RawListener&) = delete;
    RawListener& operator=(const RawListener&) = delete;
};

// Creates the native listener with ephemeral-port fallback.
std::unique_ptr<IServerTransport> make_listener(NativeRuntime& runtime, ListenerType type,
                                                uint16_t hint, uint16_t& bound,
                                                const TlsConfig* tls = nullptr) {
    ServerTransportConfig config;
    config.address = "127.0.0.1";
    if (tls) config.tls = *tls;
    for (int attempt = 0; attempt < 3; ++attempt) {
        config.listeners.clear();
        config.listeners.emplace_back(type, attempt == 0 ? hint : static_cast<uint16_t>(0));
        try {
            auto transport = runtime.make_server_transport(config);
            bound = transport->listeners().front().port;
            return transport;
        } catch (const std::system_error& error) {
            assert(attempt < 2);
            assert(error.code() == std::errc::address_in_use);
        }
    }
    return nullptr; // unreachable
}

using SharedConnection = std::shared_ptr<IConnection>;

ClientTlsConfig client_config(const std::string& server_name, const std::string& ca_file) {
    ClientTlsConfig config;
    config.server_name = server_name;
    config.ca_file = ca_file;
    config.use_system_ca = false;
    return config;
}

// ---- scenario: STARTTLS upgrade, echo + partial reads + clean close ----

void starttls_roundtrip(PollerBackend backend, uint16_t hint, const TmpDir& tmp) {
    NativeRuntime runtime(1, backend);
    runtime.start();

    // upgrade_tls before any context was configured: fail closed.
    {
        uint16_t probe_bound = 0;
        auto probe_transport = make_listener(runtime, ListenerType::TCP, hint, probe_bound);
        std::promise<std::unique_ptr<IConnection>> probe;
        auto probe_future = probe.get_future();
        probe_transport->start([&](std::unique_ptr<IConnection> connection, const ListenerConfig&) {
            probe.set_value(std::move(connection));
        });
        const int fd = open_connected_socket(probe_bound);
        assert(ready_within(probe_future, std::chrono::seconds(5)));
        auto plain = probe_future.get();
        bool threw = false;
        try {
            probe_transport->upgrade_tls(std::move(plain));
        } catch (const std::system_error& error) {
            threw = error.code() == std::errc::operation_not_supported;
        }
        assert(threw); // the refusal closes the connection we handed over
        ::close(fd);
        probe_transport->stop();
    }

    // The real STARTTLS pair, both ends native.
    uint16_t bound = 0;
    auto transport = make_listener(runtime, ListenerType::TCP, static_cast<uint16_t>(hint + 1), bound);
    transport->configure_tls(TlsConfig{tmp.file("server.pem"), tmp.file("server.key"), "", true});

    std::promise<SharedConnection> server_tls;
    auto server_ready = server_tls.get_future();
    transport->start([&](std::unique_ptr<IConnection> connection, const ListenerConfig&) {
        SharedConnection tls(transport->upgrade_tls(std::move(connection)));
        tls->async_handshake(HandshakeRole::server,
                             [&server_tls, tls](const std::error_code& error) {
                                 server_tls.set_value(error ? nullptr : tls);
                             });
    });

    auto client = runtime.make_client_transport();
    std::promise<std::error_code> connected;
    auto connected_outcome = connected.get_future();
    client->async_connect("127.0.0.1", bound,
                          [&](const std::error_code& error) { connected.set_value(error); });
    assert(ready_within(connected_outcome, std::chrono::seconds(5)));
    assert(!connected_outcome.get());

    std::promise<std::error_code> started;
    auto started_outcome = started.get_future();
    client->async_start_tls(client_config("localhost", tmp.file("server.pem")),
                            [&](const std::error_code& error) { started.set_value(error); });
    assert(ready_within(started_outcome, std::chrono::seconds(5)));
    assert(!started_outcome.get());
    assert(client->get_handshake_diagnostic().empty()); // success: no diagnostics
    assert(client->native_handle() == -1);              // raw fd hidden once TLS is on

    assert(ready_within(server_ready, std::chrono::seconds(5)));
    SharedConnection server = server_ready.get();
    assert(server);

    // Echo: whole-buffer write, exact-size read on the other side.
    const std::string message = "PING-TLS";
    std::vector<char> inbox(4096);
    std::promise<std::size_t> server_read;
    auto server_read_outcome = server_read.get_future();
    server->async_read(buffer(inbox), [&](const std::error_code& error, std::size_t bytes) {
        assert(!error);
        assert(bytes == message.size());
        assert(std::memcmp(inbox.data(), message.data(), message.size()) == 0);
        server_read.set_value(bytes);
    });
    std::promise<std::error_code> wrote;
    auto wrote_outcome = wrote.get_future();
    client->async_write(buffer(message), [&](const std::error_code& error, std::size_t bytes) {
        assert(bytes == message.size());
        wrote.set_value(error);
    });
    assert(ready_within(wrote_outcome, std::chrono::seconds(5)));
    assert(!wrote_outcome.get());
    assert(ready_within(server_read_outcome, std::chrono::seconds(5)));
    assert(server_read_outcome.get() == message.size());

    std::promise<std::error_code> echoed;
    auto echoed_outcome = echoed.get_future();
    server->async_write(buffer(message), [&](const std::error_code& error, std::size_t bytes) {
        assert(bytes == message.size());
        echoed.set_value(error);
    });
    std::promise<std::size_t> client_read;
    auto client_read_outcome = client_read.get_future();
    client->async_read(buffer(inbox), [&](const std::error_code& error, std::size_t bytes) {
        assert(!error);
        assert(bytes == message.size());
        assert(std::memcmp(inbox.data(), message.data(), message.size()) == 0);
        client_read.set_value(bytes);
    });
    assert(ready_within(echoed_outcome, std::chrono::seconds(5)));
    assert(!echoed_outcome.get());
    assert(ready_within(client_read_outcome, std::chrono::seconds(5)));
    assert(client_read_outcome.get() == message.size());

    // Partial reads: two separate records, two exact completions.
    const std::string chunk_abc = "abc";
    std::promise<std::error_code> first_write;
    auto first_write_outcome = first_write.get_future();
    client->async_write(buffer(chunk_abc),
                        [&](const std::error_code& error, std::size_t) { first_write.set_value(error); });
    assert(ready_within(first_write_outcome, std::chrono::seconds(5)));
    assert(!first_write_outcome.get());
    std::promise<std::size_t> three;
    auto three_outcome = three.get_future();
    server->async_read(buffer(inbox), [&](const std::error_code& error, std::size_t bytes) {
        assert(!error);
        assert(bytes == 3);
        three.set_value(bytes);
    });
    assert(ready_within(three_outcome, std::chrono::seconds(5)));
    assert(three_outcome.get() == 3);

    const std::string chunk_def = "defghi";
    std::promise<std::error_code> second_write;
    auto second_write_outcome = second_write.get_future();
    client->async_write(buffer(chunk_def),
                        [&](const std::error_code& error, std::size_t) { second_write.set_value(error); });
    assert(ready_within(second_write_outcome, std::chrono::seconds(5)));
    assert(!second_write_outcome.get());
    std::promise<std::size_t> six;
    auto six_outcome = six.get_future();
    server->async_read(buffer(inbox), [&](const std::error_code& error, std::size_t bytes) {
        assert(!error);
        assert(bytes == 6);
        six.set_value(bytes);
    });
    assert(ready_within(six_outcome, std::chrono::seconds(5)));
    assert(six_outcome.get() == 6);

    // Clean close: the peer's close_notify maps to EOF, never to an error.
    std::promise<std::error_code> closed;
    auto closed_outcome = closed.get_future();
    client->async_read(buffer(inbox), [&](const std::error_code& error, std::size_t bytes) {
        assert(bytes == 0);
        closed.set_value(error);
    });
    server->close();
    assert(ready_within(closed_outcome, std::chrono::seconds(5)));
    assert(closed_outcome.get() == make_error_code(IoErrc::end_of_stream));

    client->close();
    transport->stop();
    runtime.stop(true);
}

// ---- scenario: SSL listener (direct TLS, no STARTTLS) ----

void ssl_listener_roundtrip(PollerBackend backend, uint16_t hint, const TmpDir& tmp) {
    NativeRuntime runtime(1, backend);
    runtime.start();
    const TlsConfig tls{tmp.file("server.pem"), tmp.file("server.key"), "", true};
    uint16_t bound = 0;
    auto transport = make_listener(runtime, ListenerType::SSL, hint, bound, &tls);
    assert(transport->listeners().front().type == ListenerType::SSL);

    std::promise<SharedConnection> accepted;
    auto accepted_future = accepted.get_future();
    transport->start([&](std::unique_ptr<IConnection> connection, const ListenerConfig&) {
        SharedConnection tls_connection(connection.release());
        tls_connection->async_handshake(
            HandshakeRole::server,
            [&accepted, tls_connection](const std::error_code& error) {
                accepted.set_value(error ? nullptr : tls_connection);
            });
    });

    const int fd = open_connected_socket(bound);
    std::future<TlsPeer> peer_future = std::async(std::launch::async, [&] {
        return TlsPeer::connect_client(fd);
    });
    assert(ready_within(peer_future, std::chrono::seconds(5)));
    TlsPeer peer = peer_future.get();

    assert(ready_within(accepted_future, std::chrono::seconds(5)));
    SharedConnection server = accepted_future.get();
    assert(server);

    peer.write_all("FROM-PEER");
    std::vector<char> inbox(4096);
    std::promise<std::size_t> server_read;
    auto server_read_outcome = server_read.get_future();
    server->async_read(buffer(inbox), [&](const std::error_code& error, std::size_t bytes) {
        assert(!error);
        assert(bytes == 9);
        assert(std::memcmp(inbox.data(), "FROM-PEER", 9) == 0);
        server_read.set_value(bytes);
    });
    assert(ready_within(server_read_outcome, std::chrono::seconds(5)));
    assert(server_read_outcome.get() == 9);

    const std::string to_peer = "TO-PEER";
    std::promise<std::error_code> server_write;
    auto server_write_outcome = server_write.get_future();
    server->async_write(buffer(to_peer),
                        [&](const std::error_code& error, std::size_t bytes) {
                            assert(bytes == 7);
                            server_write.set_value(error);
                        });
    assert(ready_within(server_write_outcome, std::chrono::seconds(5)));
    assert(!server_write_outcome.get());
    assert(peer.read_some() == "TO-PEER");

    // Peer sends close_notify: the server must see a clean EOF.
    std::promise<std::error_code> eof;
    auto eof_outcome = eof.get_future();
    server->async_read(buffer(inbox), [&](const std::error_code& error, std::size_t bytes) {
        assert(bytes == 0);
        eof.set_value(error);
    });
    peer.clean_close();
    assert(ready_within(eof_outcome, std::chrono::seconds(5)));
    assert(eof_outcome.get() == make_error_code(IoErrc::end_of_stream));

    server->close();
    transport->stop();
    runtime.stop(true);
}

// ---- scenario: one listener distributes complete TLS connections to workers ----

void assert_on_owner(IConnection& connection, const NativeLoopPtr& owner) {
    assert(owner);
    assert(connection.executor().get() == owner.get());
    assert(owner->is_loop_thread());
}

// A one-byte exchange cannot be split into successful partial reads. All IO is
// initiated on its executor; only the test thread waits for the completions.
void worker_echo(IConnection& server, const NativeLoopPtr& owner,
                 IClientTransport& client, const NativeLoopPtr& client_owner) {
    const std::string payload = "X";
    std::vector<char> server_inbox(1);
    std::vector<char> client_inbox(1);
    std::promise<void> server_read;
    std::promise<void> server_write;
    std::promise<void> client_read;
    std::promise<void> client_write;
    auto server_read_outcome = server_read.get_future();
    auto server_write_outcome = server_write.get_future();
    auto client_read_outcome = client_read.get_future();
    auto client_write_outcome = client_write.get_future();

    owner->post([&] {
        assert_on_owner(server, owner);
        server.async_read(buffer(server_inbox), [&](const std::error_code& error, std::size_t bytes) {
            assert_on_owner(server, owner);
            assert(!error);
            assert(bytes == payload.size());
            assert(server_inbox.front() == payload.front());
            server.async_write(buffer(payload), [&](const std::error_code& error, std::size_t bytes) {
                assert_on_owner(server, owner);
                assert(!error);
                assert(bytes == payload.size());
                server_write.set_value();
            });
            server_read.set_value();
        });
    });
    client_owner->post([&] {
        assert_on_owner(client, client_owner);
        client.async_read(buffer(client_inbox), [&](const std::error_code& error, std::size_t bytes) {
            assert_on_owner(client, client_owner);
            assert(!error);
            assert(bytes == payload.size());
            assert(client_inbox.front() == payload.front());
            client_read.set_value();
        });
        client.async_write(buffer(payload), [&](const std::error_code& error, std::size_t bytes) {
            assert_on_owner(client, client_owner);
            assert(!error);
            assert(bytes == payload.size());
            client_write.set_value();
        });
    });

    assert(ready_within(server_read_outcome, std::chrono::seconds(5)));
    server_read_outcome.get();
    assert(ready_within(server_write_outcome, std::chrono::seconds(5)));
    server_write_outcome.get();
    assert(ready_within(client_read_outcome, std::chrono::seconds(5)));
    client_read_outcome.get();
    assert(ready_within(client_write_outcome, std::chrono::seconds(5)));
    client_write_outcome.get();
}

void multi_worker_tls_roundtrip(PollerBackend backend, uint16_t hint, const TmpDir& tmp,
                                ListenerType type) {
    constexpr std::size_t worker_count = 3;
    constexpr std::size_t connection_count = 2 * worker_count;
    NativeRuntime runtime(worker_count, backend);
    // Client allocation must not consume the server runtime's shared RR cursor.
    NativeRuntime client_runtime(1, backend);
    runtime.start();
    client_runtime.start();
    std::vector<NativeLoopPtr> workers;
    for (std::size_t index = 0; index < worker_count; ++index) {
        workers.push_back(runtime.loop_for(index));
        assert(workers.back());
    }
    const auto client_owner = client_runtime.loop_for(0);
    assert(client_owner);

    const TlsConfig tls{tmp.file("server.pem"), tmp.file("server.key"), "", true};
    uint16_t bound = 0;
    auto transport = make_listener(runtime, type, hint, bound, &tls);
    assert(transport->listeners().size() == 1);
    assert(transport->listeners().front().type == type);

    using AcceptPromise = std::promise<std::unique_ptr<IConnection>>;
    std::mutex accept_mutex;
    std::shared_ptr<AcceptPromise> pending_accept;
    transport->start([&](std::unique_ptr<IConnection> connection, const ListenerConfig& listener) {
        assert(listener.type == type);
        assert(listener.port == bound);
        const auto executor = connection->executor();
        NativeLoopPtr owner;
        for (const auto& worker : workers) {
            if (executor.get() == worker.get()) owner = worker;
        }
        // Checking both identity and thread rejects merely posting the handler
        // to a worker while leaving the connection/TLS session on the listener.
        assert_on_owner(*connection, owner);
        std::shared_ptr<AcceptPromise> accepted;
        {
            std::lock_guard<std::mutex> lock(accept_mutex);
            accepted = std::move(pending_accept);
        }
        assert(accepted);
        accepted->set_value(std::move(connection));
    });

    std::vector<std::size_t> owner_indices;
    std::vector<std::size_t> accepts_per_worker(worker_count, 0);
    for (std::size_t index = 0; index < connection_count; ++index) {
        auto accepted = std::make_shared<AcceptPromise>();
        auto accepted_outcome = accepted->get_future();
        {
            std::lock_guard<std::mutex> lock(accept_mutex);
            assert(!pending_accept);
            pending_accept = accepted;
        }
        auto client = client_runtime.make_client_transport();
        assert(client->executor().get() == client_owner.get());
        std::promise<void> connected;
        auto connected_outcome = connected.get_future();
        client_owner->post([&, client] {
            assert_on_owner(*client, client_owner);
            client->async_connect("127.0.0.1", bound, [&, client](const std::error_code& error) {
                assert_on_owner(*client, client_owner);
                assert(!error);
                connected.set_value();
            });
        });
        assert(ready_within(connected_outcome, std::chrono::seconds(5)));
        connected_outcome.get();
        assert(ready_within(accepted_outcome, std::chrono::seconds(5)));
        auto connection = accepted_outcome.get();
        assert(connection);
        const auto original_executor = connection->executor();
        std::size_t owner_index = worker_count;
        for (std::size_t worker = 0; worker < worker_count; ++worker) {
            if (original_executor.get() == workers[worker].get()) owner_index = worker;
        }
        assert(owner_index < worker_count);
        owner_indices.push_back(owner_index);
        ++accepts_per_worker[owner_index];
        // Listener construction may consume a cursor slot; the first accepted
        // connection establishes the offset, then two full RR cycles must follow.
        assert(owner_index == (owner_indices.front() + index) % worker_count);
        const auto owner = workers[owner_index];

        if (type == ListenerType::TCP) {
            // Exercise the actual plain stream before consuming it for STARTTLS.
            worker_echo(*connection, owner, *client, client_owner);
        }

        auto holder = std::make_shared<std::unique_ptr<IConnection>>(std::move(connection));
        std::promise<SharedConnection> handshaken;
        auto handshaken_outcome = handshaken.get_future();
        owner->post([&, holder, owner, original_executor] {
            assert_on_owner(**holder, owner);
            if (type == ListenerType::TCP) {
                *holder = transport->upgrade_tls(std::move(*holder));
                assert(*holder);
            }
            assert((*holder)->executor().get() == original_executor.get());
            SharedConnection server(std::move(*holder));
            server->async_handshake(
                HandshakeRole::server,
                [&, server, owner, original_executor](const std::error_code& error) {
                    assert_on_owner(*server, owner);
                    assert(server->executor().get() == original_executor.get());
                    assert(!error);
                    handshaken.set_value(server);
                });
        });
        std::promise<void> started;
        auto started_outcome = started.get_future();
        client_owner->post([&, client] {
            assert_on_owner(*client, client_owner);
            client->async_start_tls(client_config("localhost", tmp.file("server.pem")),
                                    [&, client](const std::error_code& error) {
                                        assert_on_owner(*client, client_owner);
                                        assert(!error);
                                        started.set_value();
                                    });
        });
        assert(ready_within(started_outcome, std::chrono::seconds(5)));
        started_outcome.get();
        assert(ready_within(handshaken_outcome, std::chrono::seconds(5)));
        auto server = handshaken_outcome.get();
        assert(server);
        assert(server->executor().get() == original_executor.get());
        worker_echo(*server, owner, *client, client_owner);

        std::promise<void> server_closed;
        std::promise<void> client_closed;
        auto server_closed_outcome = server_closed.get_future();
        auto client_closed_outcome = client_closed.get_future();
        owner->post([&, server, owner] {
            assert_on_owner(*server, owner);
            server->close();
            server_closed.set_value();
        });
        client_owner->post([&, client] {
            assert_on_owner(*client, client_owner);
            client->close();
            client_closed.set_value();
        });
        assert(ready_within(server_closed_outcome, std::chrono::seconds(5)));
        server_closed_outcome.get();
        assert(ready_within(client_closed_outcome, std::chrono::seconds(5)));
        client_closed_outcome.get();
        // No second connection is opened until this one's handshake, both IO
        // directions and close jobs have completed. No sleeps or loop-to-loop waits.
    }
    for (const auto count : accepts_per_worker) assert(count == 2);

    transport->stop();
    client_runtime.stop(true);
    runtime.stop(true);
    assert(client_runtime.exceptions_total() == 0);
    assert(runtime.exceptions_total() == 0);
}

// ---- scenario: mandatory verification failures ----

void verification_failures(PollerBackend backend, uint16_t hint, const TmpDir& tmp) {
    NativeRuntime runtime(1, backend);
    runtime.start();

    // (a) untrusted chain: the server presents `other.pem`, the client only
    // trusts `server.pem`.
    {
        uint16_t bound = 0;
        auto transport = make_listener(runtime, ListenerType::TCP, hint, bound);
        transport->configure_tls(TlsConfig{tmp.file("other.pem"), tmp.file("other.key"), "", true});
        std::promise<bool> server_rejected;
        auto server_rejected_future = server_rejected.get_future();
        transport->start([&](std::unique_ptr<IConnection> connection, const ListenerConfig&) {
            SharedConnection tls(transport->upgrade_tls(std::move(connection)));
            tls->async_handshake(HandshakeRole::server,
                                 [&server_rejected, tls](const std::error_code& error) {
                                     server_rejected.set_value(static_cast<bool>(error));
                                 });
        });
        auto client = runtime.make_client_transport();
        std::promise<std::error_code> connected;
        auto connected_outcome = connected.get_future();
        client->async_connect("127.0.0.1", bound,
                              [&](const std::error_code& e) { connected.set_value(e); });
        assert(ready_within(connected_outcome, std::chrono::seconds(5)));
        assert(!connected_outcome.get());

        std::promise<std::error_code> started;
        auto started_outcome = started.get_future();
        client->async_start_tls(client_config("localhost", tmp.file("server.pem")),
                                [&](const std::error_code& error) { started.set_value(error); });
        assert(ready_within(started_outcome, std::chrono::seconds(5)));
        assert(started_outcome.get() == make_error_code(TlsErrc::certificate_verification));
        assert(!client->get_handshake_diagnostic().empty());
        assert(ready_within(server_rejected_future, std::chrono::seconds(5)));
        assert(server_rejected_future.get()); // server side must not succeed either
        client->close();
        transport->stop();
    }

    // (b) hostname mismatch: trusted chain, wrong server name.
    {
        uint16_t bound = 0;
        auto transport =
            make_listener(runtime, ListenerType::TCP, static_cast<uint16_t>(hint + 1), bound);
        transport->configure_tls(TlsConfig{tmp.file("server.pem"), tmp.file("server.key"), "", true});
        transport->start([&](std::unique_ptr<IConnection> connection, const ListenerConfig&) {
            SharedConnection tls(transport->upgrade_tls(std::move(connection)));
            tls->async_handshake(HandshakeRole::server, [tls](const std::error_code&) {
                // Outcome ignored: the client aborts mid-handshake by design.
            });
        });
        auto client = runtime.make_client_transport();
        std::promise<std::error_code> connected;
        auto connected_outcome = connected.get_future();
        client->async_connect("127.0.0.1", bound,
                              [&](const std::error_code& e) { connected.set_value(e); });
        assert(ready_within(connected_outcome, std::chrono::seconds(5)));
        assert(!connected_outcome.get());

        std::promise<std::error_code> started;
        auto started_outcome = started.get_future();
        client->async_start_tls(client_config("wrong.host.example", tmp.file("server.pem")),
                                [&](const std::error_code& error) { started.set_value(error); });
        assert(ready_within(started_outcome, std::chrono::seconds(5)));
        assert(started_outcome.get() == make_error_code(TlsErrc::certificate_verification));
        client->close();
        transport->stop();
    }

    // (c) no trust source: the configuration is rejected up front.
    {
        uint16_t bound = 0;
        auto transport =
            make_listener(runtime, ListenerType::TCP, static_cast<uint16_t>(hint + 2), bound);
        transport->start([&](std::unique_ptr<IConnection>, const ListenerConfig&) {});
        auto client = runtime.make_client_transport();
        std::promise<std::error_code> connected;
        auto connected_outcome = connected.get_future();
        client->async_connect("127.0.0.1", bound,
                              [&](const std::error_code& e) { connected.set_value(e); });
        assert(ready_within(connected_outcome, std::chrono::seconds(5)));
        assert(!connected_outcome.get());

        ClientTlsConfig config;
        config.server_name = "localhost";
        config.use_system_ca = false; // and no ca_file / ca_pem
        std::promise<std::error_code> started;
        auto started_outcome = started.get_future();
        client->async_start_tls(config, [&](const std::error_code& error) { started.set_value(error); });
        assert(ready_within(started_outcome, std::chrono::seconds(5)));
        assert(started_outcome.get() == std::make_error_code(std::errc::protocol_error));
        assert(!client->get_handshake_diagnostic().empty());
        client->close();
        transport->stop();
    }

    runtime.stop(true);
}

// ---- scenario: close_notify vs truncation (native client, raw OpenSSL peer) ----

struct NativeToRawPair {
    std::shared_ptr<IClientTransport> client;
    std::optional<TlsPeer> peer;
};

NativeToRawPair connect_native_to_raw(NativeRuntime& runtime, const TmpDir& tmp, uint16_t port_hint) {
    RawListener listener(port_hint);
    NativeToRawPair pair;
    pair.client = runtime.make_client_transport();
    std::promise<std::error_code> connected;
    auto connected_outcome = connected.get_future();
    pair.client->async_connect("127.0.0.1", listener.port,
                               [&](const std::error_code& e) { connected.set_value(e); });
    assert(ready_within(connected_outcome, std::chrono::seconds(5)));
    assert(!connected_outcome.get());

    auto accept_future = std::async(std::launch::async, [&] {
        return TlsPeer::accept_server(listener.fd, tmp.file("server.pem"), tmp.file("server.key"));
    });
    std::promise<std::error_code> started;
    auto started_outcome = started.get_future();
    pair.client->async_start_tls(client_config("localhost", tmp.file("server.pem")),
                                 [&](const std::error_code& error) { started.set_value(error); });
    assert(ready_within(started_outcome, std::chrono::seconds(5)));
    assert(!started_outcome.get());
    assert(ready_within(accept_future, std::chrono::seconds(5)));
    pair.peer = accept_future.get();
    return pair;
}

void close_notify_vs_truncation(PollerBackend backend, uint16_t hint, const TmpDir& tmp) {
    NativeRuntime runtime(1, backend);
    runtime.start();

    // (a) truncation: the peer disappears without close_notify.
    {
        NativeToRawPair pair = connect_native_to_raw(runtime, tmp, hint);
        pair.peer->write_all("byte-1");
        std::vector<char> inbox(64);
        std::promise<std::size_t> first;
        auto first_outcome = first.get_future();
        pair.client->async_read(buffer(inbox), [&](const std::error_code& error, std::size_t bytes) {
            assert(!error);
            assert(bytes == 6);
            first.set_value(bytes);
        });
        assert(ready_within(first_outcome, std::chrono::seconds(5)));
        assert(first_outcome.get() == 6);

        std::promise<std::error_code> truncated;
        auto truncated_outcome = truncated.get_future();
        pair.client->async_read(buffer(inbox),
                                [&](const std::error_code& error, std::size_t) { truncated.set_value(error); });
        pair.peer->truncate_close(); // no close_notify: must NOT read as EOF
        assert(ready_within(truncated_outcome, std::chrono::seconds(5)));
        assert(truncated_outcome.get() == make_error_code(TlsErrc::stream_truncated));
        pair.client->close();
    }

    // (b) clean close: close_notify arrives, so the next read is EOF.
    {
        NativeToRawPair pair = connect_native_to_raw(runtime, tmp, static_cast<uint16_t>(hint + 1));
        pair.peer->write_all("byte-2");
        std::vector<char> inbox(64);
        std::promise<std::size_t> first;
        auto first_outcome = first.get_future();
        pair.client->async_read(buffer(inbox), [&](const std::error_code& error, std::size_t bytes) {
            assert(!error);
            assert(bytes == 6);
            first.set_value(bytes);
        });
        assert(ready_within(first_outcome, std::chrono::seconds(5)));
        assert(first_outcome.get() == 6);

        std::promise<std::error_code> eof;
        auto eof_outcome = eof.get_future();
        pair.client->async_read(buffer(inbox), [&](const std::error_code& error, std::size_t bytes) {
            assert(bytes == 0);
            eof.set_value(error);
        });
        pair.peer->clean_close();
        assert(ready_within(eof_outcome, std::chrono::seconds(5)));
        assert(eof_outcome.get() == make_error_code(IoErrc::end_of_stream));
        pair.client->close();
    }

    runtime.stop(true);
}

// ---- scenario: cancel exactly once + write backpressure after STARTTLS ----

void cancel_and_backpressure(PollerBackend backend, uint16_t hint, const TmpDir& tmp) {
    NativeRuntime runtime(1, backend);
    runtime.start();
    uint16_t bound = 0;
    auto transport = make_listener(runtime, ListenerType::TCP, hint, bound);
    transport->configure_tls(TlsConfig{tmp.file("server.pem"), tmp.file("server.key"), "", true});

    std::promise<SharedConnection> server_tls;
    auto server_ready = server_tls.get_future();
    transport->start([&](std::unique_ptr<IConnection> connection, const ListenerConfig&) {
        SharedConnection tls(transport->upgrade_tls(std::move(connection)));
        tls->async_handshake(HandshakeRole::server,
                             [&server_tls, tls](const std::error_code& error) {
                                 server_tls.set_value(error ? nullptr : tls);
                             });
    });

    auto client = runtime.make_client_transport();
    std::promise<std::error_code> connected;
    auto connected_outcome = connected.get_future();
    client->async_connect("127.0.0.1", bound,
                          [&](const std::error_code& e) { connected.set_value(e); });
    assert(ready_within(connected_outcome, std::chrono::seconds(5)));
    assert(!connected_outcome.get());
    // Shrink the send window BEFORE the upgrade (fd still reachable) so the
    // 4 MiB write below provably waits on the peer.
    const int send_buffer = 4096;
    assert(client->native_handle() >= 0);
    ::setsockopt(client->native_handle(), SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer));

    std::promise<std::error_code> started;
    auto started_outcome = started.get_future();
    client->async_start_tls(client_config("localhost", tmp.file("server.pem")),
                            [&](const std::error_code& error) { started.set_value(error); });
    assert(ready_within(started_outcome, std::chrono::seconds(5)));
    assert(!started_outcome.get());
    assert(ready_within(server_ready, std::chrono::seconds(5)));
    SharedConnection server = server_ready.get();
    assert(server);

    // cancel exactly once; the connection stays usable afterwards.
    {
        std::atomic<int> completions{0};
        std::promise<std::error_code> canceled;
        auto canceled_outcome = canceled.get_future();
        std::vector<char> chunk(1024);
        server->async_read(buffer(chunk), [&](const std::error_code& error, std::size_t) {
            completions.fetch_add(1);
            canceled.set_value(error);
        });
        auto executor = server->executor();
        auto watchdog = executor->make_timer();
        std::promise<void> fired;
        auto fired_outcome = fired.get_future();
        executor->post([&] {
            watchdog->expires_after(std::chrono::milliseconds(50));
            watchdog->async_wait([&](const std::error_code& error) {
                assert(!error);
                server->cancel();
                fired.set_value();
            });
        });
        assert(ready_within(canceled_outcome, std::chrono::seconds(5)));
        assert(canceled_outcome.get() == std::make_error_code(std::errc::operation_canceled));
        assert(completions.load() == 1);
        assert(ready_within(fired_outcome, std::chrono::seconds(5)));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        assert(completions.load() == 1); // exactly once

        // still usable: a fresh read receives what the client sends now.
        const std::string usable = "usable";
        std::promise<std::error_code> wrote;
        auto wrote_outcome = wrote.get_future();
        client->async_write(buffer(usable),
                            [&](const std::error_code& error, std::size_t) { wrote.set_value(error); });
        assert(ready_within(wrote_outcome, std::chrono::seconds(5)));
        assert(!wrote_outcome.get());
        std::promise<std::size_t> next;
        auto next_outcome = next.get_future();
        server->async_read(buffer(chunk), [&](const std::error_code& error, std::size_t bytes) {
            assert(!error);
            assert(bytes == 6);
            next.set_value(bytes);
        });
        assert(ready_within(next_outcome, std::chrono::seconds(5)));
        assert(next_outcome.get() == 6);
    }

    // write backpressure: completion only after the peer drains.
    {
        const std::string payload(4 << 20, 'B'); // 4 MiB
        std::promise<std::error_code> wrote;
        auto wrote_outcome = wrote.get_future();
        std::atomic<int> write_completions{0};
        client->async_write(buffer(payload), [&](const std::error_code& error, std::size_t bytes) {
            write_completions.fetch_add(1);
            assert(bytes == payload.size()); // whole buffer, exactly once
            wrote.set_value(error);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        assert(write_completions.load() == 0); // nothing drained yet

        auto total = std::make_shared<std::atomic<std::size_t>>(0);
        std::promise<std::size_t> drained;
        auto drained_outcome = drained.get_future();
        auto chunk = std::make_shared<std::vector<char>>(1 << 16);
        std::function<void()> pump_reads;
        pump_reads = [&] {
            server->async_read(buffer(*chunk), [&](const std::error_code& error, std::size_t bytes) {
                assert(!error);
                total->fetch_add(bytes);
                if (total->load() < payload.size()) pump_reads();
                else drained.set_value(total->load());
            });
        };
        pump_reads();
        assert(ready_within(wrote_outcome, std::chrono::seconds(20)));
        assert(!wrote_outcome.get());
        assert(ready_within(drained_outcome, std::chrono::seconds(20)));
        assert(drained_outcome.get() == payload.size());
        assert(write_completions.load() == 1);
    }

    client->close();
    server->close();
    transport->stop();
    runtime.stop(true);
}

} // namespace

int main() {
    assert(enable_native_tls());
    const auto backends = available_backends();
    assert(!backends.empty());
    const uint16_t hint = base_port();
    TmpDir tmp;
    const PemPair server_cert = make_self_signed("localhost", "DNS:localhost,IP:127.0.0.1");
    const PemPair other_cert = make_self_signed("other.invalid", "DNS:other.invalid");
    write_file(tmp.file("server.pem"), server_cert.cert);
    write_file(tmp.file("server.key"), server_cert.key);
    write_file(tmp.file("other.pem"), other_cert.cert);
    write_file(tmp.file("other.key"), other_cert.key);
    for (const PollerBackend backend : backends) {
        starttls_roundtrip(backend, hint, tmp);
        ssl_listener_roundtrip(backend, hint, tmp);
        multi_worker_tls_roundtrip(backend, hint, tmp, ListenerType::TCP);
        multi_worker_tls_roundtrip(backend, hint, tmp, ListenerType::SSL);
        verification_failures(backend, hint, tmp);
        close_notify_vs_truncation(backend, hint, tmp);
        cancel_and_backpressure(backend, hint, tmp);
    }
    return 0;
}
