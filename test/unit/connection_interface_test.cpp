// This translation unit must compile without ANY Boost, Asio, spdlog, or fmt
// headers. It exercises a caller-provided connection/executor implementation.
#undef NDEBUG
#include "framework/connection/i_connection.h"
#include "framework/session_base.h"
#include <cassert>
#include <deque>
#include <vector>

#if defined(BOOST_ASIO_VERSION) || defined(BOOST_VERSION) || defined(SPDLOG_VER_MAJOR) || defined(FMT_VERSION)
#error "External implementation leaked into IConnection"
#endif

class ManualExecutor final : public pr::net::IExecutor {
public:
    void post(Task task) override { tasks.push_back(std::move(task)); }
    std::shared_ptr<pr::net::ITimer> make_timer() override { return {}; }
    void drain() {
        while (!tasks.empty()) { auto task = std::move(tasks.front()); tasks.pop_front(); task(); }
    }
    std::deque<Task> tasks;
};
class CustomConnection final : public pr::IConnection {
public:
    std::shared_ptr<ManualExecutor> runtime = std::make_shared<ManualExecutor>();
    std::shared_ptr<pr::net::IExecutor> executor() override { return runtime; }
    void async_read(pr::net::MutableBuffer buffer, ReadHandler handler) override {
        runtime->post([buffer, handler = std::move(handler)] {
            if (buffer.size()) { *static_cast<char*>(buffer.data()) = 'x'; handler({}, 1); }
            else handler(pr::net::make_error_code(pr::net::IoErrc::end_of_stream), 0);
        });
    }
    void async_write(pr::net::ConstBuffer buffer, WriteHandler handler) override {
        runtime->post([buffer, handler = std::move(handler)] { handler({}, buffer.size()); });
    }
    void async_handshake(pr::net::HandshakeRole, HandshakeHandler handler) override {
        runtime->post([handler = std::move(handler)] { handler({}); });
    }
    void close() override { open = false; }
    void cancel() override {}
    bool is_open() const override { return open; }
    uint16_t get_local_port() const override { return 1234; }
    std::string get_remote_ip() const override { return "127.0.0.1"; }
private:
    bool open = true;
};
int main() {
    CustomConnection connection;
    std::vector<char> data(1);
    int calls = 0;
    connection.async_read(pr::net::buffer(data), [&](const std::error_code& error, size_t size) {
        assert(!error && size == 1 && data[0] == 'x'); ++calls;
    });
    connection.async_write_with_delay(pr::net::buffer(data), std::chrono::milliseconds(0),
        [&](const std::error_code& error, size_t size) { assert(!error && size == 1); ++calls; });
    connection.async_handshake(pr::net::HandshakeRole::server,
        [&](const std::error_code& error) { assert(!error); ++calls; });
    assert(calls == 0);
    connection.runtime->drain();
    assert(calls == 3);
    connection.close();
    assert(!connection.is_open());
}
