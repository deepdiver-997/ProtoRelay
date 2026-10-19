#ifndef PR_TEST_MANUAL_EXECUTOR_H
#define PR_TEST_MANUAL_EXECUTOR_H
#include "framework/net/executor.h"
#include <cassert>
#include <deque>
#include <vector>

namespace mail_system::test {
class ManualTimer;
class ManualExecutor final : public pr::net::IExecutor, public std::enable_shared_from_this<ManualExecutor> {
public:
    void post(Task task) override { tasks.push_back(std::move(task)); }
    std::shared_ptr<pr::net::ITimer> make_timer() override;
    void drain() {
        assert(!running);
        while (!tasks.empty()) {
            auto task = std::move(tasks.front()); tasks.pop_front();
            running = true;
            try { task(); } catch (...) { running = false; throw; }
            running = false;
        }
    }
    bool running = false;
    std::deque<Task> tasks;
    std::vector<std::shared_ptr<ManualTimer>> timers;
};

// Deterministic affinity check: DB/worker completions run outside drain().
// Any direct rearm or cancel from such a completion must fail the test.
class ManualTimer final : public pr::net::ITimer {
public:
    explicit ManualTimer(std::shared_ptr<ManualExecutor> owner) : owner_(std::move(owner)) {}
    std::shared_ptr<pr::net::IExecutor> executor() const override { return owner_.lock(); }
    void expires_after(std::chrono::milliseconds value) override {
        require_owner();
        if (handler) fire(std::make_error_code(std::errc::operation_canceled));
        delay = value;
        ++expiries;
    }
    void async_wait(Handler callback) override {
        require_owner();
        assert(!handler);
        handler = std::move(callback);
        ++waits;
    }
    void cancel() override {
        require_owner();
        ++cancels;
        if (handler) fire(std::make_error_code(std::errc::operation_canceled));
    }
    void fire(std::error_code error = {}) {
        assert(handler);
        auto callback = std::move(handler);
        handler = {};
        owner_.lock()->post([callback = std::move(callback), error] { callback(error); });
    }
    std::chrono::milliseconds delay{0};
    int expiries = 0, waits = 0, cancels = 0;
    Handler handler;
private:
    void require_owner() { assert(owner_.lock()->running && "timer mutation outside its executor"); }
    std::weak_ptr<ManualExecutor> owner_;
};

inline std::shared_ptr<pr::net::ITimer> ManualExecutor::make_timer() {
    auto timer = std::make_shared<ManualTimer>(shared_from_this());
    timers.push_back(timer);
    return timer;
}
} // namespace mail_system::test
#endif
