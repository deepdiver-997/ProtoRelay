// quic_node_loopback_test — NodeLink 跑真 lsquic 传输的整机回环（127.0.0.1 UDP）。
//
// 覆盖 quic_node brief 的 T2：
//   1. plain 链路：QUIC 握手（内嵌 TLS 1.3，自签证书 + 强制验证）→ 帧层
//      plain 握手 → ready → request/response 往返；
//   2. 服务端链路停止 → 客户端在同一条 QUIC 连接上重连开新流（无重复
//      QUIC 握手，mux 复用的真传输证明）→ 第二条服务链路 ready → 再往返；
//   3. 签名互认（protorelay_node_auth 可用时）：hello_auth + TrustStore 邻居
//      表，ready 且 peer_epoch() 透传；
//   4. 错误 CA：拨号失败，链路永远不 ready（强制验证不可关闭）。
// 双 poller 后端（kqueue/poll）各跑一轮，node 测试同款纪律。
#undef NDEBUG
#include "framework/node/node_link.h"
#include "framework/node/quic_link.h"
#ifdef QNL_HAVE_NODE_AUTH
#include "framework/node/node_auth.h"
using namespace pr::node::auth;  // NOLINT -- Ed25519KeyPair / make_credentials
#endif
#include "framework/net/native_runtime.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <string>
#include <thread>
#include <vector>

using namespace pr;        // NOLINT
using namespace pr::net;   // NOLINT
using namespace pr::node;  // NOLINT
using namespace std::chrono_literals;  // NOLINT

namespace {

template <class F>
void await(F predicate, std::chrono::milliseconds limit = 5s) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!predicate() && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(2ms);
    assert(predicate());
}

// ---- 自签证书（与 quic_loopback_test 同源：BoringSSL 无 X509V3_EXT_conf_nid） ----
struct TmpDir {
    std::filesystem::path path;
    TmpDir() {
        path = std::filesystem::temp_directory_path() /
               ("quic_node_loopback_" + std::to_string(::getpid()));
        std::filesystem::create_directories(path);
    }
    ~TmpDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    std::string file(const char* name) const { return (path / name).string(); }
};

struct PemPair { std::string cert, key; };

PemPair make_self_signed(const char* common_name, const char* sans) {
    EVP_PKEY* pkey = EVP_RSA_gen(2048);
    assert(pkey != nullptr);

    X509* x509 = X509_new(); assert(x509);
    assert(X509_set_version(x509, 2) == 1);
    assert(ASN1_INTEGER_set(X509_get_serialNumber(x509), static_cast<long>(::getpid())) == 1);
    assert(X509_gmtime_adj(X509_getm_notBefore(x509), -3600) != nullptr);
    assert(X509_gmtime_adj(X509_getm_notAfter(x509), 86400) != nullptr);
    assert(X509_set_pubkey(x509, pkey) == 1);
    X509_NAME* name = const_cast<X509_NAME*>(X509_get_subject_name(x509));
    assert(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>(common_name), -1, -1, 0) == 1);
    assert(X509_set_issuer_name(x509, name) == 1);
    X509_EXTENSION* extension =
        X509V3_EXT_nconf_nid(nullptr, nullptr, NID_subject_alt_name, const_cast<char*>(sans));
    assert(extension);
    assert(X509_add_ext(x509, extension, -1) == 1);
    X509_EXTENSION_free(extension);
    assert(X509_sign(x509, pkey, EVP_sha256()) > 0);

    PemPair pair;
    auto to_pem = [&](auto writer) {
        BIO* bio = BIO_new(BIO_s_mem()); assert(bio);
        assert(writer(bio) == 1);
        char* data = nullptr;
        const long size = BIO_get_mem_data(bio, &data);
        std::string out(data, static_cast<size_t>(size));
        BIO_free(bio);
        return out;
    };
    pair.cert = to_pem([&](BIO* bio) { return PEM_write_bio_X509(bio, x509); });
    pair.key = to_pem([&](BIO* bio) { return PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr); });
    X509_free(x509);
    EVP_PKEY_free(pkey);
    return pair;
}

void write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary);
    assert(f.good());
    f << content;
}

// ---- 链路两侧的 LinkOptions ----
LinkOptions plain_options(std::string id, std::string expected) {
    LinkOptions o;
    o.node_id = std::move(id);
    o.expected_peer_id = std::move(expected);
    o.heartbeat_interval = 200ms;
    o.heartbeat_timeout = 600ms;
    o.reconnect_min = 20ms;
    o.reconnect_max = 100ms;
    return o;
}

struct ServerPool {
    std::mutex mutex;
    std::vector<std::shared_ptr<NodeLink>> links;
    void add(std::shared_ptr<NodeLink> link) {
        std::lock_guard<std::mutex> lock(mutex);
        links.push_back(std::move(link));
    }
    size_t size() {
        std::lock_guard<std::mutex> lock(mutex);
        return links.size();
    }
    bool any_ready() {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& link : links)
            if (link->ready()) return true;
        return false;
    }
    void stop_all() {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& link : links) link->stop();
        links.clear();
    }
};

std::string request_once(const std::shared_ptr<NodeLink>& link, const std::string& payload) {
    std::promise<std::string> done;
    link->request(payload, 5s, [link, &done](std::error_code ec, std::string response) {
        assert(!ec);
        done.set_value(std::move(response));
    });
    return done.get_future().get();
}

int run_scenario(native::PollerBackend backend, bool with_auth) {
    native::NativeRuntime runtime(1, backend);
    runtime.start();
    auto loop = runtime.loop_for(0);

    TmpDir tmp;
    PemPair pem = make_self_signed("localhost", "DNS:localhost,IP:127.0.0.1");
    write_file(tmp.file("cert.pem"), pem.cert);
    write_file(tmp.file("key.pem"), pem.key);

    // 服务侧：QUIC listener → QuicStreamListener 泵 → NodeLink::accepted
    QuicServerConfig server_config;
    server_config.address = "127.0.0.1";
    server_config.port = 0;
    auto quic_server = quic::make_quic_server_transport(server_config, loop);
    TlsConfig server_tls;
    server_tls.certificate_file = tmp.file("cert.pem");
    server_tls.private_key_file = tmp.file("key.pem");

#ifdef QNL_HAVE_NODE_AUTH
    Ed25519KeyPair key_a, key_b;
    std::shared_ptr<TrustStore> trust_a, trust_b;
    LinkOptions client_opts, server_opts;
    if (with_auth) {
        key_a = generate_ed25519_key_pair();
        key_b = generate_ed25519_key_pair();
        const Ed25519KeyPair controller = generate_ed25519_key_pair();
        TrustStore::Snapshot snapshot;
        snapshot.epoch = 7;
        snapshot.neighbors.push_back({"a", key_a.public_key});
        snapshot.neighbors.push_back({"b", key_b.public_key});
        trust_a = std::make_shared<TrustStore>(controller.public_key, snapshot);
        trust_b = std::make_shared<TrustStore>(controller.public_key, snapshot);
        client_opts = plain_options("a", "b");
        client_opts.trust = trust_a;
        client_opts.credentials = make_credentials(key_a);
        server_opts = plain_options("b", "a");
        server_opts.trust = trust_b;
        server_opts.credentials = make_credentials(key_b);
    } else
#endif
    {
        client_opts = plain_options("client", "server");
        server_opts = plain_options("server", "client");
    }

    ServerPool pool;
    auto listener = QuicStreamListener::make(std::move(quic_server), server_tls, loop);
    listener->start([&pool, server_opts](std::unique_ptr<IConnection> stream) {
        auto link = NodeLink::accepted(std::move(stream), server_opts);
        // 响应器必须挂在链路创建时（echo_all 事后补挂拿不到后建链路）
        link->on_request([w = std::weak_ptr<NodeLink>(link)](uint64_t id, const std::string& payload) {
            if (auto l = w.lock()) l->respond(id, payload);
        });
        pool.add(link);
    });
    const uint16_t port = listener->config().port;
    assert(port != 0);

    // 客户侧：endpoint → NodeLink TransportFactory
    auto quic_client = quic::make_quic_client_transport(loop);
    ClientTlsConfig client_tls;
    client_tls.ca_file = tmp.file("cert.pem");
    client_tls.server_name = "localhost";
    client_tls.use_system_ca = false;
    auto endpoint = QuicLinkEndpoint::make(std::move(quic_client), client_tls, loop);

    auto client = NodeLink::client(loop, client_opts,
                                   [endpoint] { return endpoint->make_stream_transport(); });
    client->start("127.0.0.1", port);

    // 1. ready + 往返
    await([&] { return client->ready() && pool.any_ready(); });
    assert(client->peer_id() == std::string(with_auth ? "b" : "server"));
#ifdef QNL_HAVE_NODE_AUTH
    if (with_auth) assert(client->peer_epoch() == 7);
#endif
    assert(request_once(client, "ping") == "ping");

    // 2. 服务链路停止 → 客户端重连复用同一条 QUIC 连接（新流，不重握手）
    const auto attempts_before = client->connect_attempts();
    pool.stop_all();
    await([&] {
        return client->connect_attempts() > attempts_before && client->ready() &&
               pool.size() == 1 && pool.any_ready();   // stop_all 清空后重建一条
    });
    assert(request_once(client, "second") == "second");

    // 4. 错误 CA：新 endpoint 永远拨不通
    {
        ClientTlsConfig bad_tls = client_tls;
        bad_tls.ca_file = tmp.file("key.pem");   // 非证书：验证必败
        auto bad_client_transport = quic::make_quic_client_transport(loop);
        auto bad_endpoint = QuicLinkEndpoint::make(std::move(bad_client_transport), bad_tls, loop);
        auto bad_client = NodeLink::client(loop, plain_options("wrong-ca", "server"),
                                           [bad_endpoint] { return bad_endpoint->make_stream_transport(); });
        bad_client->start("127.0.0.1", port);
        std::this_thread::sleep_for(400ms);
        assert(!bad_client->ready());
        assert(bad_client->connect_attempts() >= 1);
        bad_client->stop();
    }

    client->stop();
    pool.stop_all();
    listener->stop();
    runtime.stop(true);
    return 0;
}

} // namespace

int main() {
#ifdef QNL_HAVE_NODE_AUTH
    const bool auth_available = true;
#else
    const bool auth_available = false;
#endif
#if defined(__APPLE__)
    if (run_scenario(native::PollerBackend::kqueue, false) != 0) return 1;
    if (run_scenario(native::PollerBackend::poll, false) != 0) return 1;
    if (auth_available && run_scenario(native::PollerBackend::kqueue, true) != 0) return 1;
#else
    if (run_scenario(native::PollerBackend::poll, false) != 0) return 1;
    if (auth_available && run_scenario(native::PollerBackend::poll, true) != 0) return 1;
#endif
    std::cout << "quic_node_loopback_test: all scenarios passed\n";
    return 0;
}
