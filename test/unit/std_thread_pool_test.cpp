// Standard worker semantics without Boost: cancellation must complete queued
// futures, shutdown joins active tasks, restart works, exceptions stay isolated.
#undef NDEBUG
#include "framework/thread_pool/std_thread_pool.h"
#include "framework/log/logger.h"
#include <cassert>
#include <chrono>
#include <memory>
#include <stdexcept>

using namespace std::chrono_literals;
int main() {
    pr::Logger::get_instance().set_handler({});
    bool rejected = false;
    try { pr::StdThreadPool invalid(0); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    pr::StdThreadPool pool(3);
    pool.stop();
    assert(!pool.is_running() && pool.thread_count() == 3);
    rejected = false;
    try { pool.post([] {}); } catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
    pool.start();
    pool.start();
    auto result = pool.submit([value = std::make_unique<int>(42)] { return *value; });
    assert(result.wait_for(3s) == std::future_status::ready && result.get() == 42);
    std::atomic<int> ran{0};
    std::vector<std::thread> producers;
    for (int i = 0; i < 4; ++i) producers.emplace_back([&] {
        for (int j = 0; j < 250; ++j) pool.post([&] { ++ran; });
    });
    for (auto& thread : producers) thread.join();
    pool.stop(true);
    assert(ran == 1000 && !pool.is_running());
    pool.start();
    auto restarted = pool.submit([] { return 7; });
    assert(restarted.wait_for(3s) == std::future_status::ready && restarted.get() == 7);
    pool.stop();

    pr::StdThreadPool serial(1);
    serial.start();
    serial.post([] { throw std::runtime_error("post"); });
    serial.post([] { throw 17; });
    auto failed = serial.submit([]() -> int { throw std::runtime_error("submit"); });
    assert(failed.wait_for(3s) == std::future_status::ready);
    rejected = false;
    try { (void)failed.get(); } catch (const std::runtime_error&) { rejected = true; }
    assert(rejected && serial.exceptions_total() == 2);
    auto self_stop = serial.submit([&] {
        try { serial.stop(); } catch (const std::logic_error&) { return true; }
        return false;
    });
    assert(self_stop.wait_for(3s) == std::future_status::ready && self_stop.get());

    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    auto active = serial.submit([&] { entered.set_value(); gate.wait(); return 1; });
    auto began = entered.get_future();
    assert(began.wait_for(3s) == std::future_status::ready);
    auto queued = serial.submit([] { return 2; });
    std::atomic<bool> joined{false};
    std::thread stopper([&] { serial.stop(false); joined = true; });
    assert(queued.wait_for(3s) == std::future_status::ready);
    rejected = false;
    try { (void)queued.get(); }
    catch (const std::future_error& error) { rejected = error.code() == std::make_error_code(std::future_errc::broken_promise); }
    assert(rejected && !joined && !serial.is_running());
    release.set_value();
    stopper.join();
    assert(joined && active.get() == 1);
    serial.start();
    auto final = serial.submit([] { return 3; });
    assert(final.wait_for(3s) == std::future_status::ready && final.get() == 3);
    serial.stop();
}
