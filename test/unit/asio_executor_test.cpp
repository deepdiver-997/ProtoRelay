// Adapter contract: never inline, serial execution with multiple IO runners,
// timer rearm/cancel, socket byte transfer, EOF and cancellation normalization.
#undef NDEBUG
#include "framework/connection/tcp_connection.h"
#include <atomic>
#include <cassert>
#include <future>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
int main() {
    boost::asio::io_context io;
    auto guard = boost::asio::make_work_guard(io);
    auto executor = std::make_shared<pr::net::AsioExecutor>(io.get_executor());
    int calls = 0;
    executor->post([&] { ++calls; });
    assert(calls == 0);
    io.poll();
    assert(calls == 1);
    std::thread runner1([&] { io.run(); }), runner2([&] { io.run(); });

    std::atomic<int> active{0}, count{0};
    std::vector<std::thread> producers;
    for (int i = 0; i < 4; ++i) producers.emplace_back([&] {
        for (int j = 0; j < 100; ++j) executor->post([&] {
            assert(active.fetch_add(1) == 0);
            ++count;
            assert(active.fetch_sub(1) == 1);
        });
    });
    for (auto& producer : producers) producer.join();
    std::promise<void> drained;
    executor->post([&] { assert(count == 400); drained.set_value(); });
    assert(drained.get_future().wait_for(3s) == std::future_status::ready);

    auto timer = executor->make_timer();
    std::promise<std::error_code> cancelled, expired;
    executor->post([&] {
        timer->expires_after(1h);
        timer->async_wait([&](const std::error_code& error) { cancelled.set_value(error); });
        timer->expires_after(0ms);
        timer->async_wait([&](const std::error_code& error) { expired.set_value(error); });
    });
    auto cancelled_result = cancelled.get_future(), expired_result = expired.get_future();
    assert(cancelled_result.wait_for(3s) == std::future_status::ready);
    assert(cancelled_result.get() == std::errc::operation_canceled);
    assert(expired_result.wait_for(3s) == std::future_status::ready && !expired_result.get());

    using boost::asio::ip::tcp;
    tcp::acceptor acceptor(io, tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
    tcp::socket peer(io);
    peer.connect(acceptor.local_endpoint());
    auto socket = std::make_unique<tcp::socket>(io);
    acceptor.accept(*socket);
    pr::TcpConnection connection(std::move(socket));
    auto connection_executor = connection.executor();
    std::string data(4, '\0');
    std::promise<size_t> received;
    connection_executor->post([&] {
        connection.async_read(pr::net::buffer(data.data(), 1), [&](const std::error_code& error, size_t size) {
            assert(!error); received.set_value(size);
        });
    });
    boost::asio::write(peer, boost::asio::buffer("x", 1));
    auto received_result = received.get_future();
    assert(received_result.wait_for(3s) == std::future_status::ready && received_result.get() == 1);
    assert(data[0] == 'x');
    data = "ping";
    // A delayed write must use the same serialized timer/executor path.
    std::promise<size_t> written;
    connection_executor->post([&] {
        connection.async_write_with_delay(pr::net::buffer(data), 1ms,
            [&](const std::error_code& error, size_t size) { assert(!error); written.set_value(size); });
    });
    auto written_result = written.get_future();
    assert(written_result.wait_for(3s) == std::future_status::ready && written_result.get() == 4);
    char reply[4];
    boost::asio::read(peer, boost::asio::buffer(reply));
    assert(std::string(reply, 4) == "ping");

    std::promise<std::error_code> read_cancelled;
    connection_executor->post([&] {
        connection.async_read(pr::net::buffer(data), [&](const std::error_code& error, size_t) { read_cancelled.set_value(error); });
        connection.cancel();
    });
    auto cancelled_read_result = read_cancelled.get_future();
    assert(cancelled_read_result.wait_for(3s) == std::future_status::ready);
    assert(cancelled_read_result.get() == std::errc::operation_canceled);
    peer.shutdown(tcp::socket::shutdown_send);
    std::promise<std::error_code> eof;
    connection_executor->post([&] {
        connection.async_read(pr::net::buffer(data), [&](const std::error_code& error, size_t) { eof.set_value(error); });
    });
    auto eof_result = eof.get_future();
    assert(eof_result.wait_for(3s) == std::future_status::ready);
    assert(eof_result.get() == pr::net::IoErrc::end_of_stream);
    std::promise<void> closed;
    connection_executor->post([&] { connection.close(); closed.set_value(); });
    assert(closed.get_future().wait_for(3s) == std::future_status::ready);
    guard.reset();
    runner1.join(); runner2.join();
}
