#include "framework/storage/local_storage_runtime.h"

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace pr {
namespace {

// Error reporting must not let an allocation failure escape a worker/callback path.
void set_failure(IoError& error, int code, const char* message) noexcept {
    error.kind = IoError::Kind::Retryable;
    error.code = code;
    try {
        error.message = message;
    } catch (...) {
        error.message.clear();
    }
}

std::uint64_t saturated_add(std::uint64_t a, std::uint64_t b) noexcept {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    return b > maximum - a ? maximum : a + b;
}

} // namespace

struct LocalStorageRuntime::State : std::enable_shared_from_this<State> {
    using Clock = std::chrono::steady_clock;

    struct Job {
        std::uint64_t bytes = 0;
        CommitTask task;
        CommitCallback callback;
        CompletionDispatcher dispatcher;
        Clock::time_point enqueued_at;
        Clock::time_point started_at;
        IoError error;
        bool success = false;
        bool completion_claimed = false; // protected by State::mutex
    };

    // Only a per-thread reentrancy marker, never a global runtime/executor. The
    // linked stack also detects A -> B -> A reentrancy without allocating a map.
    struct Context {
        const State* state;
        Context* previous;
        explicit Context(const State* owner) noexcept : state(owner), previous(current) {
            current = this;
        }
        ~Context() { current = previous; }
        static thread_local Context* current;
    };

    explicit State(Config value) : config(std::move(value)) {}

    bool in_context() const noexcept {
        for (auto* context = Context::current; context; context = context->previous) {
            if (context->state == this) return true;
        }
        return false;
    }

    void request_stop() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex);
            counters.admission_open = false;
            stop_requested = true;
        }
        work_available.notify_all();
    }

    void drain() {
        if (in_context()) {
            throw std::logic_error("cannot drain local storage from its own task/callback");
        }
        std::unique_lock<std::mutex> lock(mutex);
        drained.wait(lock, [this] { return counters.commits_outstanding == 0; });
    }

    void reject(CommitCallback& callback, IoError& error, int code, const char* message) {
        set_failure(error, code, message);
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++counters.commits_rejected;
        }
        Context context(this);
        try {
            if (callback) callback(false, error);
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex);
            ++counters.commit_callback_exceptions;
        }
    }

    bool submit(std::uint64_t bytes, CommitTask task, CommitCallback callback,
                CompletionDispatcher dispatcher, bool force_sync, IoError* sync_error) {
        std::shared_ptr<Job> job;
        try {
            job = std::make_shared<Job>();
        } catch (...) {
            IoError error;
            reject(callback, error, ENOMEM, "local commit allocation failed");
            if (sync_error) *sync_error = std::move(error);
            return false;
        }
        job->bytes = bytes;
        job->task = std::move(task);
        job->callback = std::move(callback);
        job->dispatcher = std::move(dispatcher);
        const bool queued = !force_sync && config.commit_mode == LocalCommitMode::Offload;
        int rejection_code = 0;
        const char* rejection_message = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!counters.admission_open) {
                rejection_code = ECANCELED;
                rejection_message = "local commit admission closed";
            } else if (!job->task) {
                rejection_code = EINVAL;
                rejection_message = "local commit task is empty";
            } else if (counters.commits_outstanding >= config.max_pending_commits ||
                       bytes > config.max_pending_commit_bytes - counters.commit_bytes_outstanding) {
                rejection_code = EAGAIN;
                rejection_message = "local commit admission capacity exceeded";
            } else {
                job->enqueued_at = Clock::now();
                if (queued) {
                    try {
                        queue.push_back(job);
                    } catch (...) {
                        rejection_code = ENOMEM;
                        rejection_message = "local commit queue allocation failed";
                    }
                }
                if (!rejection_code) {
                    ++counters.commits_accepted;
                    ++counters.commits_outstanding;
                    counters.commit_bytes_outstanding += bytes;
                    if (queued) {
                        ++counters.commits_queued;
                        counters.commit_bytes_queued += bytes;
                    } else {
                        job->started_at = job->enqueued_at;
                        ++counters.commits_running;
                        counters.commit_bytes_running += bytes;
                        // Explicit inline/sync execution has no queue residence.
                        ++counters.queue_elapsed_count;
                    }
                }
            }
        }
        if (rejection_code) {
            reject(job->callback, job->error, rejection_code, rejection_message);
            if (sync_error) *sync_error = std::move(job->error);
            return false;
        }
        if (queued) {
            work_available.notify_one();
            return true;
        }
        execute(job);
        if (sync_error) {
            *sync_error = std::move(job->error);
            return job->success;
        }
        return true;
    }

    void worker_loop() {
        // This also covers callable destruction at the end of a worker iteration.
        Context context(this);
        for (;;) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                work_available.wait(lock, [this] { return stop_requested || !queue.empty(); });
                if (queue.empty()) return;
                job = std::move(queue.front());
                queue.pop_front();
                job->started_at = Clock::now();
                const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    job->started_at - job->enqueued_at);
                ++counters.queue_elapsed_count;
                counters.queue_elapsed_total += elapsed;
                counters.queue_elapsed_max = std::max(counters.queue_elapsed_max, elapsed);
                --counters.commits_queued;
                counters.commit_bytes_queued -= job->bytes;
                ++counters.commits_running;
                counters.commit_bytes_running += job->bytes;
            }
            execute(job);
        }
    }

    void execute(const std::shared_ptr<Job>& job) {
        Context context(this);
        try {
            job->success = job->task(job->error);
            if (job->success) job->error = IoError{};
        } catch (...) {
            job->success = false;
            set_failure(job->error, EIO, "local commit task threw an exception");
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - job->started_at);
        {
            std::lock_guard<std::mutex> lock(mutex);
            --counters.commits_running;
            counters.commit_bytes_running -= job->bytes;
            ++counters.commits_completion_pending;
            counters.commit_bytes_completion_pending += job->bytes;
            ++counters.service_elapsed_count;
            counters.service_elapsed_total += elapsed;
            counters.service_elapsed_max = std::max(counters.service_elapsed_max, elapsed);
        }
        if (!job->dispatcher) {
            complete(job, false);
            return;
        }
        try {
            job->dispatcher([self = shared_from_this(), job] { self->complete(job, false); });
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++counters.commit_dispatch_failures;
            }
            // A dispatcher may enqueue then throw, or even invoke synchronously
            // then throw. The first claimant wins; late/duplicate calls are inert.
            complete(job, true);
        }
    }

    void complete(const std::shared_ptr<Job>& job, bool dispatch_failed) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (job->completion_claimed) return;
            job->completion_claimed = true;
            if (dispatch_failed) {
                job->success = false;
                set_failure(job->error, EIO, "local commit completion dispatcher threw");
            }
        }
        bool callback_threw = false;
        {
            Context context(this);
            try {
                if (job->callback) job->callback(job->success, job->error);
            } catch (...) {
                callback_threw = true;
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (callback_threw) ++counters.commit_callback_exceptions;
            if (job->success && !callback_threw) ++counters.commits_completed;
            else ++counters.commits_failed;
            --counters.commits_completion_pending;
            counters.commit_bytes_completion_pending -= job->bytes;
            --counters.commits_outstanding;
            counters.commit_bytes_outstanding -= job->bytes;
        }
        drained.notify_all();
    }

    const Config config;
    mutable std::mutex mutex;
    std::condition_variable work_available;
    std::condition_variable drained;
    Snapshot counters;
    std::deque<std::shared_ptr<Job>> queue;
    bool stop_requested = false;
};

thread_local LocalStorageRuntime::State::Context*
    LocalStorageRuntime::State::Context::current = nullptr;

struct LocalStorageRuntime::Workers {
    // Joining is serialized, but never attempted from a task/callback belonging
    // to this pool. In particular, last-owner destruction on a worker detaches
    // handles; independent shared State keeps that worker and its queue alive.
    std::mutex mutex;
    std::vector<std::thread> threads;

    void join() {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& thread : threads) {
            if (thread.joinable()) thread.join();
        }
    }

    ~Workers() {
        for (auto& thread : threads) {
            if (thread.joinable()) thread.detach();
        }
    }
};

LocalStorageRuntime::LocalStorageRuntime(Config config) {
    config.validate();
    state_ = std::make_shared<State>(std::move(config));
    workers_ = std::make_shared<Workers>();
    if (state_->config.commit_mode == LocalCommitMode::Offload) {
        try {
            workers_->threads.reserve(state_->config.commit_threads);
            for (std::size_t i = 0; i < state_->config.commit_threads; ++i) {
                workers_->threads.emplace_back([state = state_] { state->worker_loop(); });
            }
        } catch (...) {
            state_->request_stop();
            workers_->join();
            throw;
        }
    }
}

LocalStorageRuntime::~LocalStorageRuntime() {
    shutdown();
}

const LocalStorageRuntime::Config& LocalStorageRuntime::config() const noexcept {
    return state_->config;
}

LocalStorageRuntime::Snapshot LocalStorageRuntime::snapshot() const {
    const auto state = state_;
    std::lock_guard<std::mutex> lock(state->mutex);
    auto result = state->counters;
    if (!state->queue.empty()) {
        result.oldest_queue_age = std::chrono::duration_cast<std::chrono::nanoseconds>(
            State::Clock::now() - state->queue.front()->enqueued_at);
    }
    return result;
}

LocalStorageRuntime::WriteGuard::WriteGuard(std::shared_ptr<State> state,
                                          std::uint64_t requested) noexcept
    : state_(std::move(state)), requested_(requested), started_at_(std::chrono::steady_clock::now()) {}

LocalStorageRuntime::WriteGuard::~WriteGuard() {
    finish(false, 0);
}

LocalStorageRuntime::WriteGuard::WriteGuard(WriteGuard&& other) noexcept
    : state_(std::move(other.state_)), requested_(other.requested_), started_at_(other.started_at_) {}

LocalStorageRuntime::WriteGuard&
LocalStorageRuntime::WriteGuard::operator=(WriteGuard&& other) noexcept {
    if (this != &other) {
        finish(false, 0);
        state_ = std::move(other.state_);
        requested_ = other.requested_;
        started_at_ = other.started_at_;
    }
    return *this;
}

void LocalStorageRuntime::WriteGuard::finish(bool success,
                                             std::uint64_t actual_bytes_written) noexcept {
    auto state = std::move(state_);
    if (!state) return;
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started_at_);
    std::lock_guard<std::mutex> lock(state->mutex);
    ++state->counters.write_elapsed_count;
    state->counters.write_elapsed_total += elapsed;
    state->counters.write_elapsed_max = std::max(state->counters.write_elapsed_max, elapsed);
    --state->counters.writes_in_flight;
    state->counters.write_bytes_in_flight -= requested_;
    if (success) {
        ++state->counters.writes_completed;
        state->counters.write_bytes_completed += actual_bytes_written;
    } else {
        ++state->counters.writes_failed;
        state->counters.write_bytes_failed += actual_bytes_written;
    }
    state->counters.write_bytes_written += actual_bytes_written;
}

LocalStorageRuntime::WriteGuard LocalStorageRuntime::begin_write(std::uint64_t requested_bytes) {
    auto state = state_;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        ++state->counters.writes_in_flight;
        state->counters.write_bytes_in_flight += requested_bytes;
        ++state->counters.writes_total;
        state->counters.write_bytes_requested += requested_bytes;
    }
    return WriteGuard(std::move(state), requested_bytes);
}

void LocalStorageRuntime::add_uncommitted_bytes(std::uint64_t bytes) noexcept {
    const auto state = state_;
    std::lock_guard<std::mutex> lock(state->mutex);
    state->counters.uncommitted_bytes = saturated_add(state->counters.uncommitted_bytes, bytes);
}

void LocalStorageRuntime::remove_uncommitted_bytes(std::uint64_t bytes) noexcept {
    const auto state = state_;
    std::lock_guard<std::mutex> lock(state->mutex);
    auto& pending = state->counters.uncommitted_bytes;
    pending = bytes > pending ? 0 : pending - bytes;
}

bool LocalStorageRuntime::submit_commit(std::uint64_t bytes, CommitTask task,
                                        CommitCallback callback, CompletionDispatcher dispatcher) {
    const auto state = state_;
    return state->submit(bytes, std::move(task), std::move(callback), std::move(dispatcher),
                         false, nullptr);
}

bool LocalStorageRuntime::commit_sync(std::uint64_t bytes, CommitTask task, IoError& error) {
    const auto state = state_;
    return state->submit(bytes, std::move(task), {}, {}, true, &error);
}

void LocalStorageRuntime::close_admission() noexcept {
    const auto state = state_;
    std::lock_guard<std::mutex> lock(state->mutex);
    state->counters.admission_open = false;
}

void LocalStorageRuntime::drain() {
    const auto state = state_;
    state->drain();
}

void LocalStorageRuntime::shutdown() {
    const auto state = state_;
    const auto workers = workers_;
    state->request_stop();
    if (state->in_context()) return;
    state->drain();
    workers->join();
}

} // namespace pr
