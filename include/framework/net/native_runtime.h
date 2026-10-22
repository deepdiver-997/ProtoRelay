#ifndef PR_FRAMEWORK_NET_NATIVE_RUNTIME_H
#define PR_FRAMEWORK_NET_NATIVE_RUNTIME_H
// Native (non-Asio) IO runtime: task posting, serialized execution, timers,
// wakeup and shutdown on top of the native Poller abstraction.
//
// Threading model: a runtime owns N event loops; each loop is one thread with
// one Poller, one task deque, one timer wheel and one set of registered fds.
// A NativeLoop IS an IExecutor — every task, timer callback and IO completion
// runs in that loop's task-drain phase, so callbacks on one executor are
// serialized by construction and post() never executes inline (even when
// called from the loop thread itself: the task waits for the next drain).
//
// Shutdown: stop(true) marks the loop stopping, asks every registered fd
// holder and timer to cancel their pending operations (completions are queued
// like any other task), then drains the queues and joins. Pending handlers
// therefore complete with std::errc::operation_canceled exactly once before
// stop() returns. stop(false) discards the queues instead.
//
// Objects whose destruction must happen on the loop thread (connections) hand
// a disposer to defer_destroy(); it runs inline when the caller is already on
// the loop thread, is queued otherwise, and pending disposers are flushed when
// the loop thread exits or the loop is destroyed.
#include "framework/net/executor.h"
#include "framework/net/native/poller.h"
#include "framework/net/server_transport.h"
#include "framework/net/client_transport.h"
#include "framework/thread_pool/thread_pool_base.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pr::net::native {

class NativeLoop;
class NativeTimer;

// One pending timer wait. The wheel owns it as a shared_ptr; the wait's
// handler lives here, so firing or canceling never has to touch the NativeTimer
// object itself. Destroying the timer (from any thread) only sets `abandoned`:
// the wheel then drops the wait without delivering it, and no use-after-free is
// possible even when destruction races a fire.
struct TimerWait {
    std::atomic<bool> abandoned{false};
    ITimer::Handler handler; // taken by the loop before any invocation
};
using TimerWheel = std::multimap<std::chrono::steady_clock::time_point, std::shared_ptr<TimerWait>>;

// Callback set the loop keeps per registered fd. The loop invokes on_event
// with Ready bits on its own thread; on_loop_stopping is invoked exactly once
// during shutdown so the owner can cancel pending operations.
struct LoopEntry {
    std::function<void(unsigned)> on_event;
    std::function<void()> on_loop_stopping;
};

// One event loop = one thread's worth of serialized execution. Always held as
// a shared_ptr (make_timer relies on shared_from_this).
class NativeLoop final : public IExecutor, public std::enable_shared_from_this<NativeLoop> {
public:
    // poller(nullptr) creates one honoring PROTORELAY_NATIVE_POLLER.
    explicit NativeLoop(std::unique_ptr<Poller> poller = nullptr);
    ~NativeLoop() override;

    NativeLoop(const NativeLoop&) = delete;
    NativeLoop& operator=(const NativeLoop&) = delete;

    // IExecutor: never inline, serialized with everything else on this loop.
    void post(Task task) override;
    std::shared_ptr<ITimer> make_timer() override;

    // ---- fd registration (loop thread only) ----
    bool add_fd(int fd, unsigned interest, std::shared_ptr<LoopEntry> entry);
    bool mod_fd(int fd, unsigned interest);
    void remove_fd(int fd);

    // ---- timer wheel (loop thread only, driven by NativeTimer) ----
    TimerWheel::iterator arm_timer(NativeTimer& timer, std::shared_ptr<TimerWait> wait,
                                   std::chrono::steady_clock::time_point deadline);
    void cancel_timer_slot(NativeTimer& timer, TimerWheel::iterator slot);

    // ---- destruction handoff (any thread) ----
    void defer_destroy(std::function<void()> dispose);

    // Runs task on the loop thread and waits for completion; returns false
    // when no loop thread is live (the caller then runs the task inline).
    bool invoke_when_live(std::function<void()> task);

    bool is_loop_thread() const;
    void assert_loop_thread(const char* what) const;

    // ---- lifecycle ----
    // Runs until stop(); call from a dedicated thread or, for tests, inline.
    void run();
    // One iteration: wait for events, dispatch them, fire due timers, drain
    // queued tasks and deferred destructions. Test/diagnostic driver.
    void poll_once(int timeout_ms);
    // Request shutdown; wait_for_tasks selects the drain (see header comment).
    void stop(bool wait_for_tasks = true);
    bool stopping() const { return stopping_.load(std::memory_order_acquire); }

    uint64_t exceptions_total() const { return exceptions_.load(std::memory_order_relaxed); }

private:
    void enter_thread();
    void exit_thread();
    void run_iteration(int timeout_ms);
    void drain_wake_pipe();
    void drain_tasks();
    void drain_graves();
    void drain_graves_locked(); // graves_mutex_ held
    void shutdown_drain();
    void fire_due_timers();
    void loop_post_due(ITimer::Handler handler);
    int next_wait_timeout() const;

    std::unique_ptr<Poller> poller_;
    int wake_fds_[2] = {-1, -1};

    mutable std::mutex tasks_mutex_;
    std::deque<Task> tasks_;

    std::unordered_map<int, std::shared_ptr<LoopEntry>> entries_;

    TimerWheel timer_wheel_;

    // Deferred destructions from foreign threads while the loop is live.
    std::mutex graves_mutex_;
    std::deque<std::function<void()>> graves_;
    std::atomic<bool> thread_active_{false};
    std::atomic<std::thread::id> thread_id_{};

    std::atomic<bool> stopping_{false};
    std::atomic<uint64_t> exceptions_{0};
    bool drain_pending_completions_ = true; // stop(false) clears this
    bool shutdown_done_ = false;
};

// ITimer bound to one loop. At most one pending wait; expires_after/cancel of
// a pending wait completes it with operation_canceled exactly once. Handlers
// run through the loop's task queue (never inline). Destroying a timer drops
// its pending wait silently (the Asio contract) — cancel() explicitly to
// receive operation_canceled. Mutation (including destruction) belongs on the
// timer's executor; destruction from elsewhere is still memory-safe: the wait
// is merely abandoned instead of delivered.
class NativeTimer final : public ITimer {
public:
    explicit NativeTimer(std::shared_ptr<NativeLoop> loop);
    ~NativeTimer() override;

    NativeTimer(const NativeTimer&) = delete;
    NativeTimer& operator=(const NativeTimer&) = delete;

    void expires_after(std::chrono::milliseconds delay) override;
    void async_wait(Handler handler) override;
    void cancel() override;
    std::shared_ptr<IExecutor> executor() const override { return loop_; }

private:
    friend class NativeLoop;
    // Moves out the armed wait and unregisters it; returns false when no wait
    // was pending. Used by cancel(), expires_after displacement and shutdown.
    bool take_wait(std::shared_ptr<TimerWait>& out);

    std::shared_ptr<NativeLoop> loop_;
    std::shared_ptr<TimerWait> wait_; // armed wait, null otherwise
    TimerWheel::iterator slot_{};
    bool waiting_ = false; // loop-thread bookkeeping
    std::chrono::steady_clock::time_point deadline_{};
};

using NativeLoopPtr = std::shared_ptr<NativeLoop>;

// The full native runtime: N loops + threads, executors and transport
// factories. Drop-in for the IOThreadPool provider surface — production
// binaries keep using the Asio pool; this module exists in parallel.
// A stopped runtime cannot restart; construct a fresh one instead.
class NativeRuntime final : public ThreadPoolBase,
                            public IExecutorProvider,
                            public IServerTransportProvider,
                            public IClientTransportProvider,
                            public std::enable_shared_from_this<NativeRuntime> {
public:
    // backend: nullopt honors PROTORELAY_NATIVE_POLLER (default per platform).
    explicit NativeRuntime(size_t io_threads, std::optional<PollerBackend> backend = std::nullopt);
    explicit NativeRuntime(std::optional<PollerBackend> backend = std::nullopt);
    ~NativeRuntime() override;

    NativeRuntime(const NativeRuntime&) = delete;
    NativeRuntime& operator=(const NativeRuntime&) = delete;

    void start() override;
    void stop(bool wait_for_tasks = true) override;
    size_t thread_count() const override;
    bool is_running() const override;

    std::shared_ptr<IExecutor> next_executor() override;
    std::unique_ptr<IServerTransport> make_server_transport(const ServerTransportConfig& config) override;
    std::shared_ptr<IClientTransport> make_client_transport() override;

    // Worker-style posting goes round-robin across the loops.
    uint64_t exceptions_total() const;
    std::string backend() const { return backend_; }
    NativeLoopPtr loop_for(size_t index) const; // test/diagnostic access

protected:
    void post_impl(std::function<void()> task) override;

private:
    void ensure_loops();

    size_t thread_count_;
    std::string backend_;
    std::vector<NativeLoopPtr> loops_;
    std::vector<std::thread> threads_;
    std::atomic<size_t> next_loop_{0};
    std::atomic<bool> running_{false};
    bool started_once_ = false;
    mutable std::mutex lifecycle_;
};

} // namespace pr::net::native
#endif
