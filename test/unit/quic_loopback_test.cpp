// Phase C: QUIC UDP loopback availability. A minimal server + client complete
// a handshake on 127.0.0.1, open one bidirectional stream, echo one message
// and shut down cleanly with the close reason propagating to the peer.
// Driven deterministically through NativeLoop::poll_once — no threads, no
// packet loss injection (see the brief: generous timeout budget instead).
#undef NDEBUG
#include "framework/net/native_runtime.h"
#include "framework/net/quic_transport.h"

// Prefixed BoringSSL (quic_lsquic INTERFACE flags) for in-process cert
// generation — mirrors native_tls_test, minus the system OpenSSL.
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// ---- process-exclusive temp directory (AGENT_TMP_ROOT when set) ----
struct TmpDir {
    std::string path;
    TmpDir() {
        const char* configured = std::getenv("AGENT_TMP_ROOT");
        const std::string root =
            configured && *configured ? std::string(configured) : std::string("/tmp");
        std::filesystem::create_directories(root);
        std::string pattern = root + "/quic_loopback_test_XXXXXX";
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

// ---- in-process self-signed certificate (SAN carries the test names) ----
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
    // BoringSSL removed X509V3_EXT_conf_nid; nconf_nid with null CONF/ctx is
    // the same value-only parse.
    X509_EXTENSION* extension =
        X509V3_EXT_nconf_nid(nullptr, nullptr, NID_subject_alt_name, sans);
    assert(extension != nullptr);
    assert(X509_add_ext(x509, extension, -1) == 1);
    X509_EXTENSION_free(extension);
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

// ---- shared test state (all handlers run inside poll_once on this thread) ----
struct Session {
    bool server_connected = false;
    bool server_echoed = false;
    bool client_connected = false;
    bool client_echoed = false;
    std::unique_ptr<pr::net::IMuxConnection> client_connection; // for the close-reason check
    bool close_reason_seen = false;
    std::error_code close_reason_code{};
    std::string close_reason_detail;
};

const std::string kEchoMessage = "ping via quic";

// ---- client stream exchange: write one message, read the echo, close ----
void run_client_stream(Session& state, std::unique_ptr<pr::IConnection> stream) {
    using namespace pr::net;
    auto stream_box = std::make_shared<std::unique_ptr<pr::IConnection>>(std::move(stream));
    auto message = std::make_shared<std::string>(kEchoMessage);
    auto echoed = std::make_shared<std::string>();
    auto chunk = std::make_shared<std::vector<char>>(512);
    auto read_echo = std::make_shared<std::function<void(const std::error_code&, std::size_t)>>();
    *read_echo = [stream_box, message, echoed, chunk, read_echo,
                  &state](const std::error_code& error, std::size_t size) {
        assert(!error);
        echoed->append(chunk->data(), size);
        if (*echoed == *message) {
            state.client_echoed = true;
            (*stream_box)->close(); // FIN; connection closure observed by the driver
        } else {
            (*stream_box)->async_read(buffer(*chunk), *read_echo);
        }
    };
    (*stream_box)->async_write(buffer(*message),
                               [stream_box, message, chunk, read_echo](
                                   const std::error_code& error, std::size_t size) {
                                   assert(!error && size == message->size());
                                   (*stream_box)->async_read(buffer(*chunk), *read_echo);
                               });
}

int main() {
    using namespace pr::net;


    auto loop = std::make_shared<pr::net::native::NativeLoop>(nullptr);
    TmpDir tmp;
    PemPair pem = make_self_signed("localhost", "DNS:localhost,IP:127.0.0.1");
    write_file(tmp.file("cert.pem"), pem.cert);
    write_file(tmp.file("key.pem"), pem.key);

    Session state;

    // ---- server: echo one peer stream, then close the connection cleanly ----
    QuicServerConfig server_config;
    server_config.address = "127.0.0.1";
    server_config.port = 0; // ephemeral; replaced on bind
    auto server = quic::make_quic_server_transport(server_config, loop);
    {
        TlsConfig tls;
        tls.certificate_file = tmp.file("cert.pem");
        tls.private_key_file = tmp.file("key.pem");
        server->configure_tls(tls);
    }
    server->start([&state](std::unique_ptr<IMuxConnection> connection) {
        assert(connection && connection->is_open());
        state.server_connected = true;
        // Box the connection: std::function handlers need copyable captures.
        auto connection_box =
            std::make_shared<std::unique_ptr<IMuxConnection>>(std::move(connection));
        (*connection_box)
            ->accept_stream([&state, connection_box](const std::error_code& error,
                                                     std::unique_ptr<pr::IConnection> stream) {
                assert(!error && stream);
                auto stream_box = std::make_shared<std::unique_ptr<pr::IConnection>>(std::move(stream));
                auto received = std::make_shared<std::string>();
                // Read until the peer finishes (its FIN), echoing once the whole
                // message is in; then end the connection ourselves. The loop
                // owns itself through a shared_ptr (recursive handler).
                auto read_more = std::make_shared<std::function<void()>>();
                *read_more = [&state, stream_box, connection_box, received, read_more]() mutable {
                    auto chunk = std::make_shared<std::vector<char>>(512);
                    (*stream_box)
                        ->async_read(buffer(*chunk),
                                     [&state, stream_box, connection_box, received, chunk,
                                      read_more](const std::error_code& error, std::size_t size) {
                                         assert(!error || error == IoErrc::end_of_stream);
                                         if (size > 0) {
                                             received->append(chunk->data(), size);
                                             if (*received == kEchoMessage && !state.server_echoed) {
                                                 state.server_echoed = true;
                                                 (*stream_box)
                                                     ->async_write(
                                                         buffer(*received),
                                                         [read_more](const std::error_code& error,
                                                                     std::size_t size) {
                                                             assert(!error);
                                                             (void)size;
                                                             (*read_more)(); // drain to the FIN
                                                         });
                                                 return;
                                             }
                                             (*read_more)();
                                             return;
                                         }
                                         // end_of_stream: peer done — clean shutdown
                                         (*connection_box)->close();
                                     });
                };
                (*read_more)();
            });
    });

    // ---- client: connect, open a stream, exchange the echo ----
    auto client = quic::make_quic_client_transport(loop);
    {
        ClientTlsConfig tls;
        tls.ca_file = tmp.file("cert.pem");
        tls.server_name = "localhost";
        tls.use_system_ca = false;
        client->async_connect("127.0.0.1", server->config().port, tls,
                              [&state](const std::error_code& error,
                                       std::unique_ptr<IMuxConnection> connection) {
                                  assert(!error); // loopback handshake must succeed
                                  assert(connection && connection->is_open());
                                  state.client_connected = true;
                                  state.client_connection = std::move(connection);
                                  state.client_connection->open_stream(
                                      [&state](const std::error_code& error,
                                               std::unique_ptr<pr::IConnection> stream) {
                                          assert(!error); // loopback stream creation must succeed
                                          assert(stream);
                                          run_client_stream(state, std::move(stream));
                                      });
                              });
    }

    // ---- drive the loop until the close reason lands (or the budget dies) ----
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline && !state.close_reason_seen) {
        loop->poll_once(5);
        if (state.client_connected && state.client_echoed && !state.close_reason_seen &&
            state.client_connection && !state.client_connection->is_open()) {
            // server closed the connection: the reason must have propagated
            state.close_reason_code = state.client_connection->close_reason().code;
            state.close_reason_detail = state.client_connection->close_reason().detail;
            state.close_reason_seen = true;
        }
    }

    // ---- verdicts ----
    if (!state.close_reason_seen) {
        std::fprintf(stderr,
                     "quic_loopback_test: stuck — server_connected=%d server_echoed=%d "
                     "client_connected=%d client_echoed=%d\n",
                     state.server_connected, state.server_echoed, state.client_connected,
                     state.client_echoed);
        return 1;
    }
    assert(state.server_connected);
    assert(state.server_echoed);
    assert(state.client_connected);
    assert(state.client_echoed);
    // The server closed locally; the client must see a clean peer end, with a
    // non-empty reason (close-reason propagation).
    assert(state.close_reason_code == IoErrc::end_of_stream ||
           state.close_reason_code == std::error_code{});
    assert(!state.close_reason_detail.empty());
    return 0;
}
