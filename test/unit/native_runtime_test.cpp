// Native runtime contract tests: post() never inlines, one executor is one
// serialized thread, timers complete exactly once (fire, cancel, displacement,
// abandonment), and shutdown completes pending operations before returning.
// Every scenario runs on every compiled poller backend.
#undef NDEBUG
#include "framework/net/native_runtime.h"

#include <cassert>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

using namespace pr::net::native;

namespace {
template <typename T>
bool ready_within(std::future<T>& future, std::chrono::milliseconds budget) {
    return future.wait_for(budget) == std::future_status::ready;
}

// post() from outside and from the loop thread itself must never execute at
// the call site; queued tasks run in a later drain.
void post_never_inline(PollerBackend backend) {
    auto loop = std::make_shared<NativeLoop>(make_poller(backend));
    std::thread::id runner;
    loop->post([&runner] { runner = std::this_thread::get_id(); });
    assert(runner == std::thread::id{}); // nothing ran inline
    loop->poll_once(0);
    assert(runner != std::thread::id{}); // ran during the drain
    assert(runner == std::this_thread::get_id()); // poll_once runs it on the caller

    // A task posted from inside the loop waits for the NEXT iteration.
    std::atomic<int> inner{0};
    loop->post([&loop, &inner] {
        loop->post([&inner] { inner.fetch_add(1); });
        assert(inner.load() == 0); // must not run inside the same drain
    });
    loop->poll_once(0);
    assert(inner.load() == 0);
    loop->poll_once(0);
    assert(inner.load() == 1);
}

// One executor = one serialized thread, even when posters race.
void serialized_execution(PollerBackend backend) {
    NativeRuntime runtime(1, backend);
    runtime.start();
    constexpr int kTasks = 500;
    std::atomic<int> completed{0};
    std::atomic<std::thread::id> seen{std::thread::id{}};
    std::vector<std::future<void>> done;
    auto poster = [&] {
        auto executor = runtime.next_executor();
        for (int i = 0; i < kTasks; ++i) {
            auto task = std::make_shared<std::promise<void>>();
            done.push_back(task->get_future());
            executor->post([&completed, &seen, task] {
                seen.store(std::this_thread::get_id(), std::memory_order_relaxed);
                completed.fetch_add(1, std::memory_order_relaxed);
                task->set_value();
            });
        }
    };
    std::thread first(poster), second(poster);
    first.join();
    second.join();
    for (auto& future : done) assert(ready_within(future, std::chrono::seconds(5)));
    assert(completed.load() == 2 * kTasks);
    // Every task observed the same thread: the loop serializes its executor.
    const std::thread::id one = seen.load();
    assert(one != std::thread::id{});
    std::thread::id probe_id{};
    auto executor = runtime.next_executor();
    auto probe = std::make_shared<std::promise<void>>();
    auto probe_future = probe->get_future();
    executor->post([&probe_id, probe] {
        probe_id = std::this_thread::get_id();
        probe->set_value();
    });
    assert(ready_within(probe_future, std::chrono::seconds(5)));
    assert(probe_id == one);
    runtime.stop(true);
}

// A wait fires exactly once with a clean error code.
void timer_fires_once(PollerBackend backend) {
    auto loop = std::make_shared<NativeLoop>(make_poller(backend));
    auto timer = loop->make_timer();
    timer->expires_after(std::chrono::milliseconds(20));
    std::atomic<int> calls{0};
    std::promise<std::error_code> fired;
    auto outcome = fired.get_future();
    timer->async_wait([&](const std::error_code& error) {
        calls.fetch_add(1);
        fired.set_value(error);
    });
    for (int i = 0; i < 200 && !ready_within(outcome, std::chrono::milliseconds(0)); ++i)
        loop->poll_once(5);
    assert(ready_within(outcome, std::chrono::milliseconds(0)));
    assert(!outcome.get());
    assert(calls.load() == 1);
    for (int i = 0; i < 5; ++i) loop->poll_once(5);
    assert(calls.load() == 1);
}

// cancel() completes the displaced wait with operation_canceled, exactly once,
// and leaves the timer reusable.
void timer_cancel_completes_once(PollerBackend backend) {
    auto loop = std::make_shared<NativeLoop>(make_poller(backend));
    auto timer = loop->make_timer();
    timer->expires_after(std::chrono::milliseconds(5000));
    std::promise<std::error_code> canceled;
    auto outcome = canceled.get_future();
    std::atomic<int> calls{0};
    timer->async_wait([&](const std::error_code& error) {
        calls.fetch_add(1);
        canceled.set_value(error);
    });
    // cancel() belongs on the timer's executor; the loop is not running, so
    // the single-threaded test caller IS the executor between drains.
    timer->cancel();
    loop->poll_once(0);
    assert(calls.load() == 1);
    assert(ready_within(outcome, std::chrono::milliseconds(0)));
    assert(outcome.get() == std::make_error_code(std::errc::operation_canceled));
    for (int i = 0; i < 3; ++i) loop->poll_once(1);
    assert(calls.load() == 1);
    // Rearm after cancel still works.
    std::promise<std::error_code> refired;
    auto refire_outcome = refired.get_future();
    timer->expires_after(std::chrono::milliseconds(1));
    timer->async_wait([&](const std::error_code& error) { refired.set_value(error); });
    for (int i = 0; i < 200 && !ready_within(refire_outcome, std::chrono::milliseconds(0)); ++i)
        loop->poll_once(5);
    assert(ready_within(refire_outcome, std::chrono::milliseconds(0)));
    assert(!refire_outcome.get());
}

// expires_after() while a wait is pending displaces it with operation_canceled
// exactly once, then the new deadline governs.
void timer_rearm_displaces_once(PollerBackend backend) {
    auto loop = std::make_shared<NativeLoop>(make_poller(backend));
    auto timer = loop->make_timer();
    timer->expires_after(std::chrono::milliseconds(5000));
    std::promise<std::error_code> first;
    auto first_outcome = first.get_future();
    timer->async_wait([&](const std::error_code& error) { first.set_value(error); });
    timer->expires_after(std::chrono::milliseconds(1)); // displaces the pending wait
    std::promise<std::error_code> second;
    auto second_outcome = second.get_future();
    timer->async_wait([&](const std::error_code& error) { second.set_value(error); });
    for (int i = 0; i < 200 && !ready_within(second_outcome, std::chrono::milliseconds(0)); ++i)
        loop->poll_once(5);
    assert(ready_within(second_outcome, std::chrono::milliseconds(0)));
    assert(!second_outcome.get());
    assert(ready_within(first_outcome, std::chrono::milliseconds(0)));
    assert(first_outcome.get() == std::make_error_code(std::errc::operation_canceled));
    loop->poll_once(0);
    loop->poll_once(0);
}

// Destroying a timer with a pending wait drops the wait silently.
void timer_destruction_drops_wait(PollerBackend backend) {
    auto loop = std::make_shared<NativeLoop>(make_poller(backend));
    std::atomic<int> calls{0};
    {
        auto timer = loop->make_timer();
        timer->expires_after(std::chrono::milliseconds(1));
        timer->async_wait([&](const std::error_code&) { calls.fetch_add(1); });
    } // destroyed with the wait still armed
    for (int i = 0; i < 10; ++i) loop->poll_once(5);
    assert(calls.load() == 0);
}

// stop(true) completes every pending operation with operation_canceled BEFORE
// the call returns, and tasks posted before the stop all ran.
void runtime_stop_completes_pending(PollerBackend backend) {
    NativeRuntime runtime(1, backend);
    runtime.start();
    auto executor = runtime.next_executor();

    std::atomic<int> ran{0};
    for (int i = 0; i < 16; ++i)
        executor->post([&ran] { ran.fetch_add(1); });

    auto timer = std::static_pointer_cast<NativeTimer>(executor->make_timer());
    timer->expires_after(std::chrono::milliseconds(10000));
    std::promise<std::error_code> wait_outcome;
    auto outcome = wait_outcome.get_future();
    timer->async_wait([&](const std::error_code& error) { wait_outcome.set_value(error); });

    runtime.stop(true);
    assert(ran.load() == 16);
    // stop(true) waited for the queued cancelation to be delivered.
    assert(ready_within(outcome, std::chrono::milliseconds(0)));
    assert(outcome.get() == std::make_error_code(std::errc::operation_canceled));
    assert(!runtime.is_running());
    // A stopped runtime cannot restart; build a new one instead.
    bool threw = false;
    try {
        runtime.start();
    } catch (const std::logic_error&) {
        threw = true;
    }
    assert(threw);
}

} // namespace

int main() {
    for (const PollerBackend backend : available_backends()) {
        post_never_inline(backend);
        serialized_execution(backend);
        timer_fires_once(backend);
        timer_cancel_completes_once(backend);
        timer_rearm_displaces_once(backend);
        timer_destruction_drops_wait(backend);
        runtime_stop_completes_pending(backend);
    }
    return 0;
}
