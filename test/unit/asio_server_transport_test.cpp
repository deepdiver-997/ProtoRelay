// Real listener lifecycle: recover from a throwing session factory, quiesce
// accepts on stop, and preserve an accepted connection/executor across TLS.
#undef NDEBUG
#include "framework/thread_pool/io_thread_pool.h"
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <cassert>
#include <future>

using namespace std::chrono_literals;
namespace {
using tcp = boost::asio::ip::tcp;
pr::net::ServerTransportConfig config() {
    pr::net::ServerTransportConfig value;
    value.address = "127.0.0.1";
    value.listeners.emplace_back(pr::ListenerType::TCP, 0);
    return value;
}
void accepted_connection(pr::IOThreadPool& runtime) {
    auto transport = runtime.make_server_transport(config());
    auto port = transport->listeners().front().port;
    std::promise<void> first;
    std::promise<std::unique_ptr<pr::IConnection>> second;
    int calls = 0;
    transport->start([&](std::unique_ptr<pr::IConnection> connection, const pr::ListenerConfig& listener) {
        assert(listener.port == port);
        if (++calls == 1) { first.set_value(); throw std::runtime_error("session factory failed"); }
        second.set_value(std::move(connection));
    });
    boost::asio::io_context clients;
    tcp::endpoint endpoint(boost::asio::ip::make_address("127.0.0.1"), port);
    tcp::socket rejected(clients); rejected.connect(endpoint);
    assert(first.get_future().wait_for(3s) == std::future_status::ready);
    tcp::socket peer(clients); peer.connect(endpoint);
    auto result = second.get_future();
    assert(result.wait_for(3s) == std::future_status::ready);
    auto connection = result.get();
    auto executor = connection->executor();
    transport->stop(); // accepted connections and the injected IO runtime survive
    assert(runtime.is_running());
    std::string response = "still open";
    std::promise<void> sent;
    executor->post([&] {
        connection->async_write(pr::net::buffer(response), [&](const std::error_code& error, size_t size) {
            assert(!error && size == response.size()); sent.set_value();
        });
    });
    assert(sent.get_future().wait_for(3s) == std::future_status::ready);
    std::string received(response.size(), '\0');
    boost::asio::read(peer, boost::asio::buffer(received)); assert(received == response);
    std::promise<void> upgraded;
    executor->post([&] {
        connection = transport->upgrade_tls(std::move(connection));
        assert(connection->executor() == executor); // retain exactly the same strand
        upgraded.set_value();
    });
    assert(upgraded.get_future().wait_for(3s) == std::future_status::ready);
    transport.reset(); // TLS connection retains the backend's SSL context
    std::promise<void> closed;
    executor->post([&] { connection->close(); connection.reset(); closed.set_value(); });
    assert(closed.get_future().wait_for(3s) == std::future_status::ready);
}
void stopping_waits_for_accept(pr::IOThreadPool& runtime) {
    auto transport = runtime.make_server_transport(config());
    std::promise<void> entered, release, stop_entered, stopped;
    auto released = release.get_future().share();
    std::atomic<bool> completed{false};
    transport->start([&](std::unique_ptr<pr::IConnection>, const pr::ListenerConfig&) {
        entered.set_value(); released.wait(); completed = true;
    });
    boost::asio::io_context clients;
    tcp::socket peer(clients);
    peer.connect({boost::asio::ip::make_address("127.0.0.1"), transport->listeners().front().port});
    assert(entered.get_future().wait_for(3s) == std::future_status::ready);
    auto done = stopped.get_future();
    std::thread shutdown([&] { stop_entered.set_value(); transport->stop(); stopped.set_value(); });
    assert(stop_entered.get_future().wait_for(3s) == std::future_status::ready);
    assert(done.wait_for(20ms) == std::future_status::timeout);
    release.set_value(); shutdown.join();
    assert(completed.load() && done.wait_for(0ms) == std::future_status::ready);
    transport->stop(); // idempotent
}
}
int main() {
    pr::IOThreadPool runtime(2); runtime.start();
    accepted_connection(runtime);
    stopping_waits_for_accept(runtime);
    runtime.stop();
}
