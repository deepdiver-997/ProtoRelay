#undef NDEBUG
#include "framework/storage/local_storage_runtime.h"

#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

using pr::IoError;
using pr::LocalCommitMode;
using pr::LocalStorageRuntime;
using pr::LocalStorageRuntimeConfig;
using pr::LocalStorageRuntimeSnapshot;

// Timeouts only bound a broken test. All ordering is established by predicates,
// gates and joins, never sleeps or assumptions about filesystem/worker speed.
class Event {
public:
    void signal() {
        std::lock_guard<std::mutex> lock(mutex_);
        signalled_ = true;
        cv_.notify_all();
    }
    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        assert(cv_.wait_for(lock, std::chrono::seconds(10), [this] { return signalled_; }));
    }
    bool signalled() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return signalled_;
    }
private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool signalled_ = false;
};

class ManualDispatcher {
public:
    void post(std::function<void()> completion) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push_back(std::move(completion));
        cv_.notify_all();
    }
    LocalStorageRuntime::CompletionDispatcher dispatcher() {
        return [this](std::function<void()> completion) { post(std::move(completion)); };
    }
    void wait_size(std::size_t size) {
        std::unique_lock<std::mutex> lock(mutex_);
        assert(cv_.wait_for(lock, std::chrono::seconds(10),
                            [this, size] { return pending_.size() >= size; }));
    }
    std::function<void()> take() {
        std::lock_guard<std::mutex> lock(mutex_);
        assert(!pending_.empty());
        auto result = std::move(pending_.front());
        pending_.pop_front();
        return result;
    }
    void run_all() {
        for (;;) {
            std::function<void()> completion;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (pending_.empty()) return;
                completion = std::move(pending_.front());
                pending_.pop_front();
            }
            completion();
        }
    }
private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> pending_;
};

LocalStorageRuntimeConfig single_worker() {
    LocalStorageRuntimeConfig config;
    config.commit_threads = 1;
    return config;
}

void assert_consistent(const LocalStorageRuntimeSnapshot& snapshot) {
    assert(snapshot.commits_outstanding == snapshot.commits_queued + snapshot.commits_running +
                                             snapshot.commits_completion_pending);
    assert(snapshot.commit_bytes_outstanding == snapshot.commit_bytes_queued +
                                                  snapshot.commit_bytes_running +
                                                  snapshot.commit_bytes_completion_pending);
    assert(snapshot.commits_accepted == snapshot.commits_completed + snapshot.commits_failed +
                                          snapshot.commits_outstanding);
    assert(snapshot.writes_total == snapshot.writes_completed + snapshot.writes_failed +
                                       snapshot.writes_in_flight);
    assert(snapshot.write_bytes_written == snapshot.write_bytes_completed + snapshot.write_bytes_failed);
}

void test_config() {
    const LocalStorageRuntimeConfig defaults;
    defaults.validate();
    assert(defaults.commit_mode == LocalCommitMode::Offload);
    assert(defaults.commit_threads == 2);
    assert(defaults.max_pending_commits == 64);
    assert(defaults.max_pending_commit_bytes == 64ULL * 1024 * 1024);
    assert(defaults == LocalStorageRuntimeConfig{});
    for (int invalid = 0; invalid != 4; ++invalid) {
        auto config = defaults;
        switch (invalid) {
            case 0: config.commit_threads = 0; break;
            case 1: config.max_pending_commits = 0; break;
            case 2: config.max_pending_commit_bytes = 0; break;
            default: config.commit_mode = static_cast<LocalCommitMode>(99); break;
        }
        assert(config != defaults);
        bool threw = false;
        try { LocalStorageRuntime runtime(config); }
        catch (const std::invalid_argument&) { threw = true; }
        assert(threw);
    }
}

void test_write_guard_and_uncommitted() {
    auto config = single_worker();
    config.commit_mode = LocalCommitMode::Inline;
    LocalStorageRuntime runtime(config);
    assert(runtime.config() == config);
    auto first = runtime.begin_write(10);
    auto second = runtime.begin_write(20);
    auto snapshot = runtime.snapshot();
    assert(snapshot.writes_in_flight == 2 && snapshot.write_bytes_in_flight == 30);
    assert(snapshot.writes_total == 2 && snapshot.write_bytes_requested == 30);
    assert_consistent(snapshot);
    auto moved = std::move(first);
    first.finish(true, 999); // moved-from guard is inert
    moved.finish(true, 10);
    moved.finish(false, 999); // finish is idempotent
    second.finish(false, 7); // real partial pwrite bytes still count
    {
        auto third = runtime.begin_write(8);
        auto fourth = runtime.begin_write(9);
        fourth = std::move(third); // abandons the 9-byte operation
        fourth.finish(true, 8);
    }
    try {
        auto unwinding = runtime.begin_write(11);
        throw std::runtime_error("write failed before explicit finish");
    } catch (const std::runtime_error&) {}
    snapshot = runtime.snapshot();
    assert_consistent(snapshot);
    assert(snapshot.writes_total == 5 && snapshot.writes_completed == 2);
    assert(snapshot.writes_failed == 3 && snapshot.write_bytes_written == 25);
    assert(snapshot.write_bytes_completed == 18 && snapshot.write_bytes_failed == 7);
    assert(snapshot.write_bytes_requested == 58);
    assert(snapshot.writes_in_flight == 0 && snapshot.write_bytes_in_flight == 0);
    runtime.add_uncommitted_bytes(17);
    runtime.add_uncommitted_bytes(8);
    assert(runtime.snapshot().uncommitted_bytes == 25);
    runtime.remove_uncommitted_bytes(7);
    assert(runtime.snapshot().uncommitted_bytes == 18);
    runtime.remove_uncommitted_bytes(100);
    assert(runtime.snapshot().uncommitted_bytes == 0);
    runtime.add_uncommitted_bytes(std::numeric_limits<std::uint64_t>::max());
    runtime.add_uncommitted_bytes(1);
    assert(runtime.snapshot().uncommitted_bytes == std::numeric_limits<std::uint64_t>::max());
    runtime.remove_uncommitted_bytes(std::numeric_limits<std::uint64_t>::max());
    assert(runtime.snapshot().uncommitted_bytes == 0);

    // Guards retain independent accounting state, not a dangling runtime pointer.
    LocalStorageRuntime::WriteGuard surviving_guard;
    {
        LocalStorageRuntime temporary(config);
        surviving_guard = temporary.begin_write(4);
    }
    surviving_guard.finish(true, 4);
}

void test_queued_running_and_completion_admission() {
    auto config = single_worker();
    config.max_pending_commits = 3;
    config.max_pending_commit_bytes = 12;
    LocalStorageRuntime runtime(config);
    ManualDispatcher dispatcher;
    Event started, release;
    std::atomic<int> successful_callbacks{0};
    std::atomic<int> rejected_callbacks{0};
    std::atomic<int> forbidden_io{0};
    const auto callback = [&](bool success, const IoError&) {
        assert(success);
        const auto snapshot = runtime.snapshot();
        assert_consistent(snapshot);
        assert(snapshot.commits_completion_pending >= 1);
        ++successful_callbacks;
    };
    assert(runtime.submit_commit(4, [&](IoError&) {
        started.signal();
        release.wait();
        return true;
    }, callback, dispatcher.dispatcher()));
    started.wait();
    assert(runtime.submit_commit(4, [](IoError&) { return true; }, callback, dispatcher.dispatcher()));
    assert(runtime.submit_commit(4, [](IoError&) { return true; }, callback, dispatcher.dispatcher()));
    auto snapshot = runtime.snapshot();
    assert_consistent(snapshot);
    assert(snapshot.commits_running == 1 && snapshot.commit_bytes_running == 4);
    assert(snapshot.commits_queued == 2 && snapshot.commit_bytes_queued == 8);
    assert(snapshot.commits_outstanding == 3 && snapshot.commit_bytes_outstanding == 12);
    assert(snapshot.oldest_queue_age.count() >= 0);
    assert(snapshot.service_elapsed_count == 0);
    const auto rejection = [&](bool success, const IoError& error) {
        assert(!success && error.retryable() && error.code == EAGAIN);
        ++rejected_callbacks;
    };
    assert(!runtime.submit_commit(0, [&](IoError&) { ++forbidden_io; return true; }, rejection));
    release.signal();
    dispatcher.wait_size(3);
    snapshot = runtime.snapshot();
    assert_consistent(snapshot);
    assert(snapshot.commits_queued == 0 && snapshot.commits_running == 0);
    assert(snapshot.commits_completion_pending == 3 && snapshot.commit_bytes_completion_pending == 12);
    assert(snapshot.service_elapsed_count == 3 && snapshot.queue_elapsed_count == 3);
    assert(snapshot.oldest_queue_age.count() == 0);
    assert(snapshot.queue_elapsed_total >= snapshot.queue_elapsed_max);
    assert(snapshot.service_elapsed_total >= snapshot.service_elapsed_max);
    assert(!runtime.submit_commit(1, [&](IoError&) { ++forbidden_io; return true; }, rejection));
    dispatcher.take()();
    assert(runtime.snapshot().commits_outstanding == 2);
    assert(runtime.submit_commit(4, [](IoError&) { return true; }, callback, dispatcher.dispatcher()));
    dispatcher.wait_size(3);
    dispatcher.run_all();
    runtime.drain();
    snapshot = runtime.snapshot();
    assert_consistent(snapshot);
    assert(snapshot.commits_accepted == 4 && snapshot.commits_completed == 4);
    assert(snapshot.commits_rejected == 2 && snapshot.commits_failed == 0);
    assert(snapshot.commits_outstanding == 0 && snapshot.commit_bytes_outstanding == 0);
    assert(snapshot.queue_elapsed_count == 4 && snapshot.service_elapsed_count == 4);
    assert(successful_callbacks == 4 && rejected_callbacks == 2 && forbidden_io == 0);
}

void test_byte_limit_and_zero_byte_commits() {
    auto config = single_worker();
    config.max_pending_commits = 8;
    config.max_pending_commit_bytes = 5;
    LocalStorageRuntime runtime(config);
    ManualDispatcher dispatcher;
    std::atomic<int> io{0}, callbacks{0}, rejections{0};
    const auto done = [&](bool success, const IoError&) { assert(success); ++callbacks; };
    const auto rejected = [&](bool success, const IoError& error) {
        assert(!success && error.code == EAGAIN);
        ++rejections;
    };
    assert(runtime.submit_commit(5, [&](IoError&) { ++io; return true; }, done, dispatcher.dispatcher()));
    dispatcher.wait_size(1);
    assert(runtime.submit_commit(0, [&](IoError&) { ++io; return true; }, done, dispatcher.dispatcher()));
    dispatcher.wait_size(2);
    assert(!runtime.submit_commit(1, [&](IoError&) { ++io; return true; }, rejected));
    assert(!runtime.submit_commit(6, [&](IoError&) { ++io; return true; }, rejected));
    assert(!runtime.submit_commit(std::numeric_limits<std::uint64_t>::max(),
                                 [&](IoError&) { ++io; return true; }, rejected));
    const auto snapshot = runtime.snapshot();
    assert_consistent(snapshot);
    assert(snapshot.commits_outstanding == 2 && snapshot.commit_bytes_outstanding == 5);
    dispatcher.run_all();
    runtime.drain();
    assert(io == 2 && callbacks == 2 && rejections == 3);
    IoError error;
    assert(runtime.commit_sync(5, [&](IoError&) { ++io; return true; }, error));
    assert(io == 3 && runtime.snapshot().commit_bytes_outstanding == 0);
}

void test_inline_and_reentrant_admission() {
    auto config = single_worker();
    config.commit_mode = LocalCommitMode::Inline;
    config.max_pending_commits = 1;
    LocalStorageRuntime runtime(config);
    const auto caller = std::this_thread::get_id();
    int callbacks = 0, forbidden_io = 0;
    assert(runtime.submit_commit(2, [&](IoError&) {
        assert(std::this_thread::get_id() == caller);
        const auto snapshot = runtime.snapshot();
        assert_consistent(snapshot);
        assert(snapshot.commits_running == 1 && snapshot.commits_queued == 0);
        IoError error;
        assert(!runtime.commit_sync(1, [&](IoError&) { ++forbidden_io; return true; }, error));
        assert(error.code == EAGAIN);
        return true;
    }, [&](bool success, const IoError&) {
        assert(success && std::this_thread::get_id() == caller);
        const auto snapshot = runtime.snapshot();
        assert_consistent(snapshot);
        assert(snapshot.commits_running == 0 && snapshot.commits_completion_pending == 1);
        assert(!runtime.submit_commit(1, [&](IoError&) { ++forbidden_io; return true; },
            [&](bool nested_success, const IoError&) { assert(!nested_success); ++callbacks; }));
        bool drain_threw = false;
        try { runtime.drain(); } catch (const std::logic_error&) { drain_threw = true; }
        assert(drain_threw);
        ++callbacks;
    }));
    assert(callbacks == 2 && forbidden_io == 0);
    auto snapshot = runtime.snapshot();
    assert_consistent(snapshot);
    assert(snapshot.commits_accepted == 1 && snapshot.commits_completed == 1);
    assert(snapshot.commits_rejected == 2 && snapshot.commits_outstanding == 0);
    IoError error = IoError::permanent("old error", EACCES);
    assert(runtime.commit_sync(1, [](IoError&) { return true; }, error));
    assert(error.message.empty() && error.code == 0);
    snapshot = runtime.snapshot();
    assert(snapshot.queue_elapsed_count == 2 && snapshot.queue_elapsed_total.count() == 0);
    assert(snapshot.service_elapsed_count == 2);
}

void test_explicit_sync_inside_offload_worker() {
    auto config = single_worker();
    config.max_pending_commits = 2;
    LocalStorageRuntime runtime(config);
    const auto caller = std::this_thread::get_id();
    IoError error;
    assert(runtime.commit_sync(1, [&](IoError&) {
        assert(std::this_thread::get_id() == caller);
        return true;
    }, error));
    std::atomic<int> callbacks{0};
    assert(runtime.submit_commit(1, [&](IoError&) {
        const auto worker = std::this_thread::get_id();
        assert(worker != caller);
        IoError nested_error;
        assert(runtime.commit_sync(1, [&](IoError&) {
            assert(std::this_thread::get_id() == worker);
            assert(runtime.snapshot().commits_running == 2);
            return true;
        }, nested_error));
        return true;
    }, [&](bool success, const IoError&) { assert(success); ++callbacks; }));
    runtime.drain();
    const auto snapshot = runtime.snapshot();
    assert_consistent(snapshot);
    assert(callbacks == 1 && snapshot.commits_completed == 3 && snapshot.commits_rejected == 0);
}

void test_callback_reentry_with_available_capacity() {
    auto config = single_worker();
    config.max_pending_commits = 2;
    LocalStorageRuntime runtime(config);
    ManualDispatcher dispatcher;
    std::atomic<int> callbacks{0};
    assert(runtime.submit_commit(4, [](IoError&) { return true; },
        [&](bool success, const IoError&) {
            assert(success);
            IoError error;
            assert(runtime.commit_sync(2, [&](IoError&) {
                const auto snapshot = runtime.snapshot();
                assert_consistent(snapshot);
                assert(snapshot.commits_running == 1 && snapshot.commits_completion_pending == 1);
                assert(snapshot.commits_outstanding == 2);
                return true;
            }, error));
            assert(runtime.submit_commit(1, [](IoError&) { return true; },
                [&](bool nested_success, const IoError&) { assert(nested_success); ++callbacks; }));
            ++callbacks;
        }, dispatcher.dispatcher()));
    dispatcher.wait_size(1);
    dispatcher.run_all();
    runtime.drain();
    const auto snapshot = runtime.snapshot();
    assert_consistent(snapshot);
    assert(callbacks == 2 && snapshot.commits_completed == 3 && snapshot.commits_rejected == 0);
}

void test_failures_exceptions_and_worker_survival() {
    LocalStorageRuntime runtime(single_worker());
    std::atomic<int> callbacks{0};
    assert(runtime.submit_commit(1, [](IoError& error) {
        error = IoError::permanent("denied", EACCES);
        return false;
    }, [&](bool success, const IoError& error) {
        assert(!success && !error.retryable() && error.code == EACCES);
        ++callbacks;
    }));
    assert(runtime.submit_commit(1, [](IoError&) -> bool {
        throw std::runtime_error("fsync threw");
    }, [&](bool success, const IoError& error) {
        assert(!success && error.retryable() && error.code == EIO);
        ++callbacks;
    }));
    assert(runtime.submit_commit(1, [](IoError&) { return true; }, [&](bool success, const IoError&) {
        assert(success);
        ++callbacks;
        throw std::runtime_error("callback threw");
    }));
    assert(runtime.submit_commit(1, [](IoError&) { return true; }, [&](bool success, const IoError&) {
        assert(success);
        ++callbacks;
    }));
    runtime.drain();
    auto snapshot = runtime.snapshot();
    assert_consistent(snapshot);
    assert(callbacks == 4 && snapshot.commits_failed == 3 && snapshot.commits_completed == 1);
    assert(snapshot.commit_callback_exceptions == 1 && snapshot.commits_outstanding == 0);
    IoError error;
    assert(!runtime.commit_sync(1, [](IoError&) -> bool { throw 7; }, error));
    assert(error.retryable() && error.code == EIO);
    assert(!runtime.submit_commit(1, {}, [&](bool success, const IoError& invalid) {
        assert(!success && invalid.code == EINVAL);
        ++callbacks;
        throw std::runtime_error("rejected callback threw");
    }));
    snapshot = runtime.snapshot();
    assert_consistent(snapshot);
    assert(callbacks == 5 && snapshot.commits_rejected == 1);
    assert(snapshot.commit_callback_exceptions == 2 && snapshot.commits_failed == 4);
}

void test_dispatcher_failure_duplicate_and_late_invocation() {
    LocalStorageRuntime runtime(single_worker());
    ManualDispatcher late;
    std::atomic<int> failed_callbacks{0}, successful_callbacks{0}, io{0};
    assert(runtime.submit_commit(3, [&](IoError&) { ++io; return true; },
        [&](bool success, const IoError& error) {
            assert(!success && error.retryable() && error.code == EIO);
            assert(runtime.snapshot().commits_running == 0);
            ++failed_callbacks;
        }, [](std::function<void()>) { throw std::runtime_error("dispatch refused"); }));
    assert(runtime.submit_commit(3, [&](IoError&) { ++io; return true; },
        [&](bool success, const IoError& error) {
            assert(!success && error.retryable());
            ++failed_callbacks;
        }, [&](std::function<void()> completion) {
            late.post(std::move(completion));
            throw std::runtime_error("enqueued then threw");
        }));
    assert(runtime.submit_commit(3, [&](IoError&) { ++io; return true; },
        [&](bool success, const IoError&) { assert(success); ++successful_callbacks; },
        [](std::function<void()> completion) {
            completion();
            completion();
            throw std::runtime_error("already delivered then threw");
        }));
    runtime.drain();
    // The final dispatcher throws AFTER invoking its callback. Tokens may already
    // be drained at that instant, so join the worker before inspecting diagnostics.
    runtime.shutdown();
    assert(failed_callbacks == 2 && successful_callbacks == 1 && io == 3);
    auto stale = late.take();
    stale();
    stale();
    const auto snapshot = runtime.snapshot();
    assert_consistent(snapshot);
    assert(failed_callbacks == 2 && successful_callbacks == 1);
    assert(snapshot.commit_dispatch_failures == 3 && snapshot.commits_failed == 2);
    assert(snapshot.commits_completed == 1 && snapshot.commits_outstanding == 0);
}

void test_close_drain_and_executing_completion() {
    LocalStorageRuntime runtime(single_worker());
    ManualDispatcher dispatcher;
    Event callback_entered, release_callback, drain_entered, drain_done;
    std::atomic<int> callbacks{0}, forbidden_io{0}, forbidden_dispatch{0};
    assert(runtime.submit_commit(4, [](IoError&) { return true; },
        [&](bool success, const IoError&) {
            assert(success);
            callback_entered.signal();
            release_callback.wait();
            assert(!drain_done.signalled());
            ++callbacks;
        }, dispatcher.dispatcher()));
    dispatcher.wait_size(1);
    runtime.close_admission();
    runtime.close_admission();
    assert(!runtime.snapshot().admission_open);
    const auto caller = std::this_thread::get_id();
    assert(!runtime.submit_commit(4, [&](IoError&) { ++forbidden_io; return true; },
        [&](bool success, const IoError& error) {
            assert(!success && error.code == ECANCELED && error.retryable());
            assert(std::this_thread::get_id() == caller);
            ++callbacks;
        }, [&](std::function<void()>) { ++forbidden_dispatch; }));
    IoError error;
    assert(!runtime.commit_sync(4, [&](IoError&) { ++forbidden_io; return true; }, error));
    assert(error.code == ECANCELED);
    std::thread drainer([&] {
        drain_entered.signal();
        runtime.drain();
        drain_done.signal();
    });
    drain_entered.wait();
    assert(!drain_done.signalled());
    std::thread completer([&] { dispatcher.take()(); });
    callback_entered.wait();
    const auto snapshot = runtime.snapshot();
    assert_consistent(snapshot);
    assert(snapshot.commits_running == 0 && snapshot.commits_completion_pending == 1);
    assert(snapshot.commits_outstanding == 1 && snapshot.commit_bytes_outstanding == 4);
    assert(!drain_done.signalled());
    release_callback.signal();
    completer.join();
    drainer.join();
    assert(drain_done.signalled());
    assert(callbacks == 2 && forbidden_io == 0 && forbidden_dispatch == 0);
    runtime.shutdown();
    runtime.shutdown();
}

void test_shutdown_waits_for_dispatched_completion() {
    LocalStorageRuntime runtime(single_worker());
    ManualDispatcher dispatcher;
    Event entered, done;
    assert(runtime.submit_commit(1, [](IoError&) { return true; },
                                 [](bool success, const IoError&) { assert(success); },
                                 dispatcher.dispatcher()));
    dispatcher.wait_size(1);
    std::thread stopper([&] { entered.signal(); runtime.shutdown(); done.signal(); });
    entered.wait();
    assert(!done.signalled());
    assert(runtime.snapshot().commits_outstanding == 1);
    dispatcher.run_all();
    stopper.join();
    assert(done.signalled());
    assert(!runtime.snapshot().admission_open);
}

void test_worker_shutdown_keeps_accepted_queue() {
    LocalStorageRuntime runtime(single_worker());
    Event entered, release, stopped;
    std::atomic<int> callbacks{0};
    assert(runtime.submit_commit(1, [&](IoError&) { entered.signal(); release.wait(); return true; },
        [&](bool success, const IoError&) {
            assert(success);
            runtime.shutdown(); // must neither join itself nor wait for this callback
            ++callbacks;
            stopped.signal();
        }));
    entered.wait();
    assert(runtime.submit_commit(1, [](IoError&) { return true; },
                                 [&](bool success, const IoError&) { assert(success); ++callbacks; }));
    release.signal();
    stopped.wait();
    runtime.drain();
    runtime.shutdown();
    const auto snapshot = runtime.snapshot();
    assert_consistent(snapshot);
    assert(callbacks == 2 && snapshot.commits_completed == 2 && !snapshot.admission_open);
}

void test_last_owner_destroyed_on_worker() {
    auto runtime = std::make_shared<LocalStorageRuntime>(single_worker());
    std::weak_ptr<LocalStorageRuntime> weak = runtime;
    auto entered = std::make_shared<Event>();
    auto release = std::make_shared<Event>();
    auto destroyed = std::make_shared<Event>();
    auto second_completed = std::make_shared<Event>();
    assert(runtime->submit_commit(1,
        [entered, release](IoError&) { entered->signal(); release->wait(); return true; },
        [owner = runtime, destroyed](bool success, const IoError&) mutable {
            assert(success);
            owner.reset(); // last owner: runtime destructor executes on its worker
            destroyed->signal();
        }));
    entered->wait();
    assert(runtime->submit_commit(1, [](IoError&) { return true; },
        [second_completed](bool success, const IoError&) {
            assert(success);
            second_completed->signal();
        }));
    runtime.reset();
    assert(!weak.expired());
    release->signal();
    destroyed->wait();
    assert(weak.expired());
    second_completed->wait(); // accepted queue outlives the destroyed facade
}

void test_instance_isolation() {
    auto config = single_worker();
    config.max_pending_commits = 1;
    LocalStorageRuntime first(config), second(config);
    Event entered, release;
    assert(first.submit_commit(1, [&](IoError&) { entered.signal(); release.wait(); return true; }, {}));
    entered.wait();
    first.add_uncommitted_bytes(17);
    auto write = first.begin_write(5);
    IoError error;
    assert(second.commit_sync(1, [](IoError&) { return true; }, error));
    first.close_admission();
    assert(second.submit_commit(1, [](IoError&) { return true; }, {}));
    second.drain();
    const auto one = first.snapshot();
    const auto two = second.snapshot();
    assert_consistent(one);
    assert_consistent(two);
    assert(one.commits_outstanding == 1 && one.writes_in_flight == 1);
    assert(one.uncommitted_bytes == 17 && !one.admission_open);
    assert(two.commits_completed == 2 && two.writes_total == 0);
    assert(two.uncommitted_bytes == 0 && two.admission_open);
    write.finish(true, 5);
    release.signal();
    first.drain();
}

void test_concurrent_admission_is_atomic() {
    auto config = single_worker();
    config.max_pending_commits = 2;
    config.max_pending_commit_bytes = 6;
    LocalStorageRuntime runtime(config);
    Event submit, release_io;
    std::atomic<int> accepted{0}, rejected{0}, io{0}, callbacks{0};
    std::vector<std::thread> submitters;
    for (int i = 0; i != 8; ++i) {
        submitters.emplace_back([&] {
            submit.wait();
            if (runtime.submit_commit(3, [&](IoError&) {
                ++io;
                release_io.wait();
                return true;
            }, [&](bool success, const IoError& error) {
                if (!success) { assert(error.code == EAGAIN); ++rejected; }
                ++callbacks;
            })) ++accepted;
        });
    }
    submit.signal();
    for (auto& submitter : submitters) submitter.join();
    const auto snapshot = runtime.snapshot();
    assert_consistent(snapshot);
    assert(accepted == 2 && rejected == 6 && snapshot.commits_outstanding == 2);
    assert(snapshot.commit_bytes_outstanding == 6 && snapshot.commits_rejected == 6);
    release_io.signal();
    runtime.drain();
    assert(io == 2 && callbacks == 8 && runtime.snapshot().commits_completed == 2);
}

} // namespace

int main() {
    std::printf("local_storage_runtime_test\n");
    test_config();
    test_write_guard_and_uncommitted();
    test_queued_running_and_completion_admission();
    test_byte_limit_and_zero_byte_commits();
    test_inline_and_reentrant_admission();
    test_explicit_sync_inside_offload_worker();
    test_callback_reentry_with_available_capacity();
    test_failures_exceptions_and_worker_survival();
    test_dispatcher_failure_duplicate_and_late_invocation();
    test_close_drain_and_executing_completion();
    test_shutdown_waits_for_dispatched_completion();
    test_worker_shutdown_keeps_accepted_queue();
    test_last_owner_destroyed_on_worker();
    test_instance_isolation();
    test_concurrent_admission_is_atomic();
    std::printf("  all local storage runtime assertions passed\n");
    return 0;
}
