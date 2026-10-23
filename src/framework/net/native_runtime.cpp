#include "framework/net/native_runtime.h"
#include "framework/net/native_transport.h"
#include "framework/log/log_macros.h"

#include <algorithm>
#include <future>
#include <cerrno>
#include <climits>
#include <cstring>
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>

namespace pr::net::native {
namespace {
std::error_code last_error() { return std::error_code(errno, std::generic_category()); }

void set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

size_t default_loop_count() {
    const unsigned hardware = std::thread::hardware_concurrency();
    return std::max<size_t>(1, std::min<size_t>(hardware ? hardware : 1, 4));
}
} // namespace

// ---------------------------------------------------------------- NativeLoop

NativeLoop::NativeLoop(std::unique_ptr<Poller> poller) : poller_(std::move(poller)) {
    if (!poller_) poller_ = make_poller_from_env();
    if (!poller_) throw std::runtime_error("no native poller backend available");
    if (::pipe(wake_fds_) != 0) throw std::system_error(last_error(), "native loop wake pipe");
    set_nonblocking(wake_fds_[0]);
    set_nonblocking(wake_fds_[1]);
    poller_->add(wake_fds_[0], Ready::readable);
}

NativeLoop::~NativeLoop() {
    stop(false);
    // Corpses queued from foreign threads after the final drain are destroyed
    // here; the loop outlives them because connections hold a shared_ptr to
    // their loop.
    drain_graves();
    if (wake_fds_[0] >= 0) ::close(wake_fds_[0]);
    if (wake_fds_[1] >= 0) ::close(wake_fds_[1]);
}

void NativeLoop::post(Task task) {
    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        tasks_.push_back(std::move(task));
    }
    // A queued byte keeps the wake fd readable until the loop drains the pipe,
    // so a wakeup posted between push and wait can never be lost.
    const char token = 'w';
    ssize_t ignored = ::write(wake_fds_[1], &token, 1);
    (void)ignored;
}

std::shared_ptr<ITimer> NativeLoop::make_timer() {
    return std::make_shared<NativeTimer>(shared_from_this());
}

bool NativeLoop::add_fd(int fd, unsigned interest, std::shared_ptr<LoopEntry> entry) {
    assert_loop_thread("add_fd");
    if (fd < 0 || !entry || entries_.count(fd)) return false;
    const bool registered = poller_->add(fd, interest);
    if (!registered) return false;
    entries_.emplace(fd, std::move(entry));
    return true;
}

bool NativeLoop::mod_fd(int fd, unsigned interest) {
    assert_loop_thread("mod_fd");
    if (!entries_.count(fd)) return false;
    return poller_->mod(fd, interest);
}

void NativeLoop::remove_fd(int fd) {
    assert_loop_thread("remove_fd");
    entries_.erase(fd);
    poller_->remove(fd);
}

TimerWheel::iterator NativeLoop::arm_timer(NativeTimer& timer, std::shared_ptr<TimerWait> wait,
                                           std::chrono::steady_clock::time_point deadline) {
    assert_loop_thread("arm_timer");
    return timer_wheel_.emplace(deadline, std::move(wait));
}

void NativeLoop::cancel_timer_slot(NativeTimer&, TimerWheel::iterator slot) {
    assert_loop_thread("cancel_timer_slot");
    timer_wheel_.erase(slot);
}

void NativeLoop::defer_destroy(std::function<void()> dispose) {
    if (!dispose) return;
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(graves_mutex_);
        if (thread_active_.load(std::memory_order_acquire)) {
            graves_.push_back(std::move(dispose));
            queued = true;
        }
    }
    if (queued) {
        post(nullptr); // wake only; nullptr tasks are skipped by the drain
    } else {
        dispose();
    }
}

bool NativeLoop::is_loop_thread() const {
    return thread_active_.load(std::memory_order_acquire) &&
           thread_id_.load(std::memory_order_acquire) == std::this_thread::get_id();
}

void NativeLoop::assert_loop_thread(const char* what) const {
    // Enforcement is about concurrency: a live loop thread must be the only
    // mutator. When no thread is running (stopped loop, test-driven
    // poll_once), a single foreign caller is serialized by definition.
    if (thread_active_.load(std::memory_order_acquire) && !is_loop_thread())
        throw std::logic_error(std::string("native loop misuse: ") + what +
                               " outside the loop thread");
}

// Runs task on the loop thread and blocks until it finished. Returns false
// when no loop thread is live (caller runs the task inline instead). The
// graves_mutex_ covers both the liveness check and the enqueue so a loop
// thread cannot exit between them and strand the caller's completion.
bool NativeLoop::invoke_when_live(std::function<void()> task) {
    if (is_loop_thread()) { // never wait on ourselves
        try { task(); } catch (...) {}
        return true;
    }
    auto done = std::make_shared<std::promise<void>>();
    auto finished = done->get_future();
    {
        std::lock_guard<std::mutex> lock(graves_mutex_);
        if (!thread_active_.load(std::memory_order_acquire)) return false;
        {
            std::lock_guard<std::mutex> task_lock(tasks_mutex_);
            tasks_.push_back([task = std::move(task), done] {
                try {
                    task();
                } catch (...) {
                    // Teardown jobs must not strand the waiting thread.
                }
                done->set_value();
            });
        }
        const char token = 'w';
        ssize_t ignored = ::write(wake_fds_[1], &token, 1);
        (void)ignored;
    }
    finished.wait();
    return true;
}

void NativeLoop::run() {
    enter_thread();
    while (!stopping_.load(std::memory_order_acquire)) run_iteration(-1);
    shutdown_drain();
    exit_thread();
}

void NativeLoop::poll_once(int timeout_ms) {
    // A caller may drive the loop without a thread; the transient ownership
    // makes is_loop_thread() and the graves logic work in that mode too.
    const bool transient_owner = !thread_active_.load(std::memory_order_acquire);
    if (transient_owner) enter_thread();
    if (stopping_.load(std::memory_order_acquire)) shutdown_drain();
    else run_iteration(timeout_ms);
    if (transient_owner) exit_thread();
}

void NativeLoop::stop(bool wait_for_tasks) {
    drain_pending_completions_ = wait_for_tasks;
    stopping_.store(true, std::memory_order_release);
    post(nullptr);
}

void NativeLoop::run_iteration(int timeout_ms) {
    // Block until the earlier of the caller's cap and the next timer deadline
    // (-1 = no bound); an empty wheel must not turn a finite cap into forever.
    const int timer_wait = next_wait_timeout();
    int effective = timer_wait;
    if (timeout_ms >= 0 && (effective < 0 || timeout_ms < effective)) effective = timeout_ms;
    std::vector<PollEvent> events;
    poller_->wait(effective, events);

    // Resolve fd -> entry up front: a callback that closes its own fd (or any
    // other) cannot invalidate the iteration, and a stale event can never
    // reach a handler registered later on the same descriptor number.
    std::vector<std::pair<std::shared_ptr<LoopEntry>, unsigned>> dispatch;
    bool woke = false;
    for (const auto& event : events) {
        if (event.fd == wake_fds_[0]) { woke = true; continue; }
        const auto found = entries_.find(event.fd);
        if (found == entries_.end()) continue;
        dispatch.emplace_back(found->second, event.ready);
    }
    if (woke) drain_wake_pipe();
    for (auto& dispatched : dispatch) {
        const auto& entry = dispatched.first;
        if (!entry->on_event) continue;
        try {
            entry->on_event(dispatched.second);
        } catch (const std::exception& error) {
            exceptions_.fetch_add(1, std::memory_order_relaxed);
            LOG_NETWORK_ERROR("Native loop fd callback threw: {}", error.what());
        } catch (...) {
            exceptions_.fetch_add(1, std::memory_order_relaxed);
            LOG_NETWORK_ERROR("Native loop fd callback threw a non-standard exception");
        }
    }
    fire_due_timers();
    drain_tasks(false);
    drain_graves();
}

void NativeLoop::drain_wake_pipe() {
    char sink[256];
    for (;;) {
        const ssize_t got = ::read(wake_fds_[0], sink, sizeof(sink));
        if (got > 0) continue;
        break; // EAGAIN: drained (or an unexpected error; the next post re-wakes)
    }
}

void NativeLoop::fire_due_timers() {
    const auto now = std::chrono::steady_clock::now();
    // Erase from the wheel before delivering: the callback may arm new waits
    // or destroy other timers, and no node may be dispatched twice. The node
    // shared_ptr carries the handler, so no NativeTimer state is touched here.
    while (!timer_wheel_.empty() && timer_wheel_.begin()->first <= now) {
        const auto node = timer_wheel_.begin();
        const std::shared_ptr<TimerWait> wait = node->second;
        timer_wheel_.erase(node);
        if (wait->abandoned.load(std::memory_order_acquire)) continue; // timer destroyed
        ITimer::Handler handler;
        handler.swap(wait->handler);
        loop_post_due(std::move(handler));
    }
}

void NativeLoop::loop_post_due(ITimer::Handler handler) {
    post([handler = std::move(handler)] { handler(std::error_code{}); });
}

void NativeLoop::drain_tasks(bool everything) {
    // Normal iteration drains the SNAPSHOT of tasks queued when the drain
    // starts; a task posted during the drain waits for the NEXT iteration.
    // This is lossless: its post() wrote a wake byte that this iteration did
    // not consume, so the blocked wait still wakes. Draining fewer tasks than
    // were queued WOULD lose wakeups (their bytes are already gone from the
    // pipe), and draining everything would let self-posting tasks starve IO
    // and timers. Shutdown (everything=true) keeps draining until the queue
    // is truly empty.
    for (;;) {
        std::deque<Task> batch;
        {
            std::lock_guard<std::mutex> lock(tasks_mutex_);
            if (tasks_.empty()) return;
            // Snapshot: everything queued NOW. Tasks posted during the run
            // stay queued and woke the poll with their own bytes.
            batch.swap(tasks_);
        }
        while (!batch.empty()) {
            Task task = std::move(batch.front());
            batch.pop_front();
            if (!task) continue;
            try {
                task();
            } catch (const std::exception& error) {
                exceptions_.fetch_add(1, std::memory_order_relaxed);
                LOG_NETWORK_ERROR("Native loop task threw: {}", error.what());
            } catch (...) {
                exceptions_.fetch_add(1, std::memory_order_relaxed);
                LOG_NETWORK_ERROR("Native loop task threw a non-standard exception");
            }
        }
        if (!everything) return;
    }
}

void NativeLoop::drain_graves() {
    std::lock_guard<std::mutex> lock(graves_mutex_);
    drain_graves_locked();
}

void NativeLoop::drain_graves_locked() {
    while (!graves_.empty()) {
        auto dispose = std::move(graves_.front());
        graves_.pop_front();
        try {
            dispose();
        } catch (const std::exception& error) {
            LOG_NETWORK_ERROR("Native loop deferred destruction threw: {}", error.what());
        } catch (...) {
            LOG_NETWORK_ERROR("Native loop deferred destruction threw a non-standard exception");
        }
    }
}

int NativeLoop::next_wait_timeout() const {
    if (timer_wheel_.empty()) return -1;
    const auto now = std::chrono::steady_clock::now();
    const auto head = timer_wheel_.begin()->first;
    if (head <= now) return 0;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(head - now).count();
    return static_cast<int>(std::min<long long>(remaining, INT_MAX));
}

void NativeLoop::shutdown_drain() {
    if (shutdown_done_) return;
    // Cancel every armed timer wait first: their canceled completions join the
    // task queue and are delivered by the drain below, exactly once each.
    while (!timer_wheel_.empty()) {
        const auto node = timer_wheel_.begin();
        const std::shared_ptr<TimerWait> wait = node->second;
        timer_wheel_.erase(node);
        if (wait->abandoned.load(std::memory_order_acquire)) continue;
        ITimer::Handler handler;
        handler.swap(wait->handler);
        post([handler = std::move(handler)] {
            handler(std::make_error_code(std::errc::operation_canceled));
        });
    }
    // Give every registered fd holder the chance to cancel pending operations;
    // completions are queued like any other task and delivered by the drain.
    std::vector<std::shared_ptr<LoopEntry>> stopping;
    stopping.reserve(entries_.size());
    for (auto& entry : entries_) stopping.push_back(entry.second);
    for (auto& entry : stopping) {
        if (!entry->on_loop_stopping) continue;
        try {
            entry->on_loop_stopping();
        } catch (const std::exception& error) {
            LOG_NETWORK_ERROR("Native loop shutdown callback threw: {}", error.what());
        } catch (...) {
            LOG_NETWORK_ERROR("Native loop shutdown callback threw a non-standard exception");
        }
    }
    if (drain_pending_completions_) {
        // Graves may queue canceled completions (connection disposal during
        // shutdown): keep alternating until both queues are exhausted so every
        // pending operation completes before the thread leaves.
        for (;;) {
            drain_tasks(true);
            drain_graves();
            {
                std::lock_guard<std::mutex> tasks_lock(tasks_mutex_);
                std::lock_guard<std::mutex> graves_lock(graves_mutex_);
                if (tasks_.empty() && graves_.empty()) break;
            }
        }
    }
    shutdown_done_ = true;
}

void NativeLoop::enter_thread() {
    std::lock_guard<std::mutex> lock(graves_mutex_);
    thread_id_.store(std::this_thread::get_id(), std::memory_order_release);
    thread_active_.store(true, std::memory_order_release);
}

void NativeLoop::exit_thread() {
    std::lock_guard<std::mutex> lock(graves_mutex_);
    // Deactivation happens only after the final grave sweep: a foreign
    // destructor that observed thread_active_ gets its corpse collected here.
    drain_graves_locked();
    thread_active_.store(false, std::memory_order_release);
}

// ---------------------------------------------------------------- NativeTimer

NativeTimer::NativeTimer(std::shared_ptr<NativeLoop> loop) : loop_(std::move(loop)) {
    if (!loop_) throw std::invalid_argument("native timer requires a loop");
}

NativeTimer::~NativeTimer() {
    // A destroyed timer silently drops its pending wait (mirrors Asio); call
    // cancel() explicitly to receive operation_canceled. Abandonment is a
    // plain atomic store: the loop may concurrently be firing this wait, and
    // it only ever touches the TimerWait node, never this object.
    if (wait_) wait_->abandoned.store(true, std::memory_order_release);
}

void NativeTimer::expires_after(std::chrono::milliseconds delay) {
    loop_->assert_loop_thread("timer expires_after");
    std::shared_ptr<TimerWait> displaced;
    if (take_wait(displaced)) {
        ITimer::Handler handler;
        handler.swap(displaced->handler);
        loop_->post([handler = std::move(handler)] {
            handler(std::make_error_code(std::errc::operation_canceled));
        });
    }
    deadline_ = std::chrono::steady_clock::now() + delay;
}

void NativeTimer::async_wait(Handler handler) {
    loop_->assert_loop_thread("timer async_wait");
    if (waiting_) throw std::logic_error("native timer already has a pending wait");
    if (!handler) throw std::invalid_argument("native timer wait requires a handler");
    waiting_ = true;
    wait_ = std::make_shared<TimerWait>();
    wait_->handler = std::move(handler);
    slot_ = loop_->arm_timer(*this, wait_, deadline_);
}

void NativeTimer::cancel() {
    loop_->assert_loop_thread("timer cancel");
    std::shared_ptr<TimerWait> displaced;
    if (!take_wait(displaced)) return;
    ITimer::Handler handler;
    handler.swap(displaced->handler);
    loop_->post([handler = std::move(handler)] {
        handler(std::make_error_code(std::errc::operation_canceled));
    });
}

bool NativeTimer::take_wait(std::shared_ptr<TimerWait>& out) {
    if (!waiting_) return false;
    loop_->cancel_timer_slot(*this, slot_);
    waiting_ = false;
    out.swap(wait_);
    return true;
}

// -------------------------------------------------------------- NativeRuntime

NativeRuntime::NativeRuntime(size_t io_threads, std::optional<PollerBackend> backend)
    : thread_count_(std::max<size_t>(1, io_threads)) {
    if (backend) {
        if (!backend_available(*backend))
            throw std::invalid_argument("native poller backend not compiled into this binary: " +
                                        backend_name(*backend));
        backend_ = backend_name(*backend);
    }
    ensure_loops(); // loops exist before start(): tests may drive poll_once
}

NativeRuntime::NativeRuntime(std::optional<PollerBackend> backend)
    : NativeRuntime(default_loop_count(), backend) {}

NativeRuntime::~NativeRuntime() { stop(true); }

void NativeRuntime::ensure_loops() {
    if (!loops_.empty()) return;
    for (size_t i = 0; i < thread_count_; ++i) {
        std::unique_ptr<Poller> poller;
        if (!backend_.empty()) poller = make_poller(backend_from_name(backend_));
        loops_.push_back(std::make_shared<NativeLoop>(std::move(poller)));
    }
}

void NativeRuntime::start() {
    std::lock_guard<std::mutex> lock(lifecycle_);
    if (running_.load(std::memory_order_acquire)) return;
    if (started_once_)
        throw std::logic_error("a stopped native runtime cannot restart; construct a new one");
    ensure_loops();
    for (const auto& loop : loops_) threads_.emplace_back([loop] { loop->run(); });
    running_.store(true, std::memory_order_release);
    started_once_ = true;
}

void NativeRuntime::stop(bool wait_for_tasks) {
    std::vector<std::thread> joining;
    {
        std::lock_guard<std::mutex> lock(lifecycle_);
        for (auto& loop : loops_) loop->stop(wait_for_tasks);
        joining.swap(threads_);
        running_.store(false, std::memory_order_release);
    }
    for (auto& thread : joining) thread.join();
}

size_t NativeRuntime::thread_count() const { return thread_count_; }
bool NativeRuntime::is_running() const { return running_.load(std::memory_order_acquire); }

std::shared_ptr<IExecutor> NativeRuntime::next_executor() {
    std::lock_guard<std::mutex> lock(lifecycle_);
    ensure_loops();
    const size_t index = next_loop_.fetch_add(1, std::memory_order_relaxed) % loops_.size();
    return loops_[index];
}

uint64_t NativeRuntime::exceptions_total() const {
    std::lock_guard<std::mutex> lock(lifecycle_);
    uint64_t total = 0;
    for (const auto& loop : loops_) total += loop->exceptions_total();
    return total;
}

NativeLoopPtr NativeRuntime::loop_for(size_t index) const {
    std::lock_guard<std::mutex> lock(lifecycle_);
    if (loops_.empty() || index >= loops_.size()) return {};
    return loops_[index];
}

std::unique_ptr<IServerTransport> NativeRuntime::make_server_transport(const ServerTransportConfig& config) {
    std::lock_guard<std::mutex> lock(lifecycle_);
    ensure_loops();
    // The transport only needs the loops (shared_ptr); it stays usable even if
    // the runtime object itself is released first.
    return make_native_server_transport(config, loops_);
}

std::shared_ptr<IClientTransport> NativeRuntime::make_client_transport() {
    return make_native_client_transport(next_executor());
}

void NativeRuntime::post_impl(std::function<void()> task) {
    if (task) next_executor()->post(std::move(task));
}

} // namespace pr::net::native
