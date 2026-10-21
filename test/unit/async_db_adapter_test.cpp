#undef NDEBUG
#include <cassert>
#include "framework/db/async_db_adapter.h"
#include <deque>

namespace {
class Executor : public pr::net::IExecutor {
public:
    void post(Task task) override { tasks.push_back(std::move(task)); }
    std::shared_ptr<pr::net::ITimer> make_timer() override { return {}; }
    void run() { while (!tasks.empty()) { auto task = std::move(tasks.front()); tasks.pop_front(); task(); } }
    std::deque<Task> tasks;
};
class Workers : public pr::ThreadPoolBase {
public:
    void start() override { running = true; }
    void stop(bool = true) override { running = false; }
    size_t thread_count() const override { return 1; }
    bool is_running() const override { return running; }
    void run() { while (!tasks.empty()) { auto task = std::move(tasks.front()); tasks.pop_front(); task(); } }
    std::deque<std::function<void()>> tasks;
protected:
    void post_impl(std::function<void()> task) override {
        if (!running) throw std::runtime_error("stopped");
        tasks.push_back(std::move(task));
    }
private:
    bool running = true;
};
class Driver : public pr::IDBConnection {
public:
    Dialect dialect() const override { return Dialect::Null; }
    bool connect() override { return true; }
    void disconnect() override {}
    bool is_connected() const override { return true; }
    std::shared_ptr<pr::IDBResult> query(const std::string&) override { ++queries; return {}; }
    std::shared_ptr<pr::IDBResult> query(const std::string& s, const std::vector<std::string>&) override { return query(s); }
    bool execute(const std::string& sql) override { ++executions; if (sql == "throw") throw std::runtime_error("failed"); return true; }
    bool execute(const std::string& s, const std::vector<std::string>&) override { return execute(s); }
    bool begin_transaction() override { return true; }
    bool commit() override { return true; }
    bool rollback() override { return true; }
    std::string get_last_error() const override { return {}; }
    std::string escape_string(const std::string& s) const override { return s; }
    int queries = 0, executions = 0;
};
}
int main() {
    auto driver = std::make_shared<Driver>();
    bool inline_called = false;
    driver->async_execute("inline", [&](bool success) { assert(success); inline_called = true; });
    assert(inline_called); // the existing interface deliberately retains this contract
    auto workers = std::make_shared<Workers>();
    auto executor = std::make_shared<Executor>();
    auto adapter = std::make_shared<pr::AsyncDBConnectionAdapter>(driver, workers, executor);
    bool completed = false;
    adapter->async_execute("worker", [&](bool success) {
        assert(success && !adapter->async_in_flight()); completed = true;
    });
    assert(!completed && driver->executions == 1 && adapter->async_in_flight());
    bool rejected = false;
    adapter->async_execute("overlap", [&](bool success) { assert(!success); rejected = true; });
    executor->run();
    assert(rejected && adapter->async_in_flight());
    workers->run();
    assert(!completed && adapter->async_in_flight() && driver->executions == 2);
    executor->run();
    assert(completed && !adapter->async_in_flight());
    bool failed = false;
    adapter->async_execute("throw", [&](bool success) { assert(!success); failed = true; });
    workers->run(); executor->run();
    assert(failed && !adapter->async_in_flight());
    workers->stop();
    bool stopped = false;
    adapter->async_commit([&](bool success) { assert(!success); stopped = true; });
    assert(!stopped);
    executor->run();
    assert(stopped && !adapter->async_in_flight());
    workers->start();
    bool alive = false;
    adapter->async_rollback([&](bool success) { assert(success); alive = true; });
    std::weak_ptr<pr::AsyncDBConnectionAdapter> weak = adapter;
    adapter.reset();
    assert(!weak.expired());
    workers->run(); executor->run();
    assert(alive && weak.expired());
}
