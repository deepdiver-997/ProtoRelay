#ifndef PR_FRAMEWORK_STORAGE_LOCAL_STORAGE_RUNTIME_H
#define PR_FRAMEWORK_STORAGE_LOCAL_STORAGE_RUNTIME_H

#include "framework/storage/io_error.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>

namespace pr {

enum class LocalCommitMode { Inline, Offload };

struct LocalStorageRuntimeConfig {
    LocalCommitMode commit_mode = LocalCommitMode::Offload;
    std::size_t commit_threads = 2;
    std::size_t max_pending_commits = 64;
    std::uint64_t max_pending_commit_bytes = 64ULL * 1024 * 1024;

    // Keep configuration-only consumers independent of the storage library.
    void validate() const {
        if (commit_mode != LocalCommitMode::Inline && commit_mode != LocalCommitMode::Offload)
            throw std::invalid_argument("invalid local storage commit mode");
        if (commit_threads == 0 || max_pending_commits == 0 || max_pending_commit_bytes == 0)
            throw std::invalid_argument("local storage commit threads and limits must be positive");
    }
    bool operator==(const LocalStorageRuntimeConfig& other) const noexcept {
        return commit_mode == other.commit_mode && commit_threads == other.commit_threads &&
               max_pending_commits == other.max_pending_commits &&
               max_pending_commit_bytes == other.max_pending_commit_bytes;
    }
    bool operator!=(const LocalStorageRuntimeConfig& other) const noexcept {
        return !(*this == other);
    }
};

struct LocalStorageRuntimeSnapshot {
    bool admission_open = true;

    std::uint64_t writes_in_flight = 0;
    std::uint64_t write_bytes_in_flight = 0; // requested bytes of active guards
    std::uint64_t writes_total = 0;
    std::uint64_t writes_completed = 0; // successful operations
    std::uint64_t writes_failed = 0;
    std::uint64_t write_bytes_requested = 0;
    std::uint64_t write_bytes_written = 0; // completed + failed actual bytes
    std::uint64_t write_bytes_completed = 0; // actual bytes from successful guards
    std::uint64_t write_bytes_failed = 0; // partial actual bytes from failed guards
    std::uint64_t uncommitted_bytes = 0; // application accounting, NOT kernel dirty pages
    std::uint64_t write_elapsed_count = 0;
    std::chrono::nanoseconds write_elapsed_total{0};
    std::chrono::nanoseconds write_elapsed_max{0};

    std::uint64_t commits_queued = 0;
    std::uint64_t commit_bytes_queued = 0;
    std::uint64_t commits_running = 0;
    std::uint64_t commit_bytes_running = 0;
    // Includes a callback currently executing, until it returns (or throws).
    std::uint64_t commits_completion_pending = 0;
    std::uint64_t commit_bytes_completion_pending = 0;
    std::uint64_t commits_outstanding = 0;
    std::uint64_t commit_bytes_outstanding = 0;
    std::uint64_t commits_accepted = 0;
    std::uint64_t commits_completed = 0; // successful task AND callback delivery
    std::uint64_t commits_failed = 0; // accepted task, dispatch or callback failure
    std::uint64_t commits_rejected = 0;
    std::uint64_t commit_dispatch_failures = 0;
    std::uint64_t commit_callback_exceptions = 0;

    std::uint64_t queue_elapsed_count = 0;
    std::chrono::nanoseconds queue_elapsed_total{0};
    std::chrono::nanoseconds queue_elapsed_max{0};
    std::uint64_t service_elapsed_count = 0;
    std::chrono::nanoseconds service_elapsed_total{0};
    std::chrono::nanoseconds service_elapsed_max{0};
    std::chrono::nanoseconds oldest_queue_age{0};
};

class LocalStorageRuntime {
    struct State;
    struct Workers;

public:
    using Config = LocalStorageRuntimeConfig;
    using Snapshot = LocalStorageRuntimeSnapshot;
    using CommitTask = std::function<bool(IoError&)>;
    using CommitCallback = std::function<void(bool, const IoError&)>;
    using CompletionDispatcher = std::function<void(std::function<void()>)>;

    class WriteGuard {
    public:
        WriteGuard() noexcept = default;
        ~WriteGuard();
        WriteGuard(WriteGuard&& other) noexcept;
        WriteGuard& operator=(WriteGuard&& other) noexcept;
        WriteGuard(const WriteGuard&) = delete;
        WriteGuard& operator=(const WriteGuard&) = delete;

        // Idempotent. Destruction without finish counts as a failed, zero-byte write.
        void finish(bool success, std::uint64_t actual_bytes_written) noexcept;

    private:
        friend class LocalStorageRuntime;
        WriteGuard(std::shared_ptr<State> state, std::uint64_t requested) noexcept;
        std::shared_ptr<State> state_;
        std::uint64_t requested_ = 0;
        std::chrono::steady_clock::time_point started_at_{};
    };

    explicit LocalStorageRuntime(Config config = {});
    ~LocalStorageRuntime();
    LocalStorageRuntime(const LocalStorageRuntime&) = delete;
    LocalStorageRuntime& operator=(const LocalStorageRuntime&) = delete;
    LocalStorageRuntime(LocalStorageRuntime&&) = delete;
    LocalStorageRuntime& operator=(LocalStorageRuntime&&) = delete;

    const Config& config() const noexcept;
    Snapshot snapshot() const;
    WriteGuard begin_write(std::uint64_t requested_bytes);
    void add_uncommitted_bytes(std::uint64_t bytes) noexcept;
    // Saturates at zero, making abort/cleanup safe after a partial write.
    void remove_uncommitted_bytes(std::uint64_t bytes) noexcept;

    // true means admitted, not necessarily successful. Limits cover queued + running
    // + completion-pending operations/bytes. No rejected task is ever executed.
    // Every submission invokes its callback exactly once (if nonempty), including
    // rejection. Rejection callbacks run synchronously, without using dispatcher.
    // A task exception becomes a retryable EIO failure. A throwing dispatcher causes
    // a retryable completion failure delivered directly, never inline I/O fallback.
    // The completion is guarded against duplicate or late dispatcher invocation.
    // First delivery wins: if dispatcher invokes the callback and THEN throws,
    // diagnostics count the dispatch failure but no second result is delivered.
    // Callback exceptions are swallowed, counted as failed accepted commits, and
    // release admission tokens (a rejected callback exception remains a rejection).
    // Dispatchers must eventually invoke/retain their closure or throw; drain waits
    // for externally dispatched completions and cannot pump their event loop.
    bool submit_commit(std::uint64_t bytes, CommitTask task, CommitCallback callback,
                       CompletionDispatcher dispatcher = {});

    // Explicit caller-thread execution, regardless of commit_mode. Uses the same
    // atomic admission and counters; never waits on a dedicated worker/future.
    bool commit_sync(std::uint64_t bytes, CommitTask task, IoError& error);

    void close_admission() noexcept;
    // Waits for accepted commits INCLUDING completion callbacks, not active writes.
    // Calling from this runtime's task/callback throws std::logic_error rather than
    // deadlocking on the caller's own token.
    void drain();
    // Close and drain, then stop/join the dedicated workers. From this runtime's
    // task/callback: request graceful stop without waiting; accepted work still runs.
    // Destruction on a worker is safe: workers retain State, not a raw runtime pointer.
    void shutdown();

private:
    std::shared_ptr<State> state_;
    std::shared_ptr<Workers> workers_;
};

} // namespace pr

#endif // PR_FRAMEWORK_STORAGE_LOCAL_STORAGE_RUNTIME_H
