#include "framework/storage/local_file_write_stream.h"
#include "framework/storage/local_storage_runtime.h"

#include <cerrno>
#include <filesystem>
#include <limits>
#include <mutex>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace pr {

struct LocalFileWriteStream::State {
    enum class Phase { Open, Writing, Queued, Committing, Persisted, Committed, Failed, Aborted };

    State(int handle, std::string name, std::shared_ptr<LocalStorageRuntime> owner, Operations ops)
        : fd(handle), path(std::move(name)), runtime(std::move(owner)), operations(std::move(ops)) {}
    ~State() { abort(); }

    void release_uncommitted_locked() noexcept {
        if (runtime) runtime->remove_uncommitted_bytes(uncommitted_bytes);
        uncommitted_bytes = 0;
    }
    void close_locked() noexcept {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
    void abort_locked() noexcept {
        close_locked();
        if (phase != Phase::Committed) ::unlink(path.c_str());
        release_uncommitted_locked();
        phase = Phase::Aborted;
    }
    void abort() noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        if (phase == Phase::Committed || phase == Phase::Aborted) return;
        abort_requested = true;
        if (phase != Phase::Writing && phase != Phase::Committing) abort_locked();
    }
    void finish_write(std::size_t written, bool success) noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        uncommitted_bytes += written;
        if (runtime) runtime->add_uncommitted_bytes(written);
        if (abort_requested) abort_locked();
        else phase = success ? Phase::Open : Phase::Failed;
    }
    // The caller has already selected Queued, preventing a new write/commit.
    bool persist(IoError& error) {
        int handle;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (phase == Phase::Aborted || abort_requested) {
                error = IoError::retryable("local commit cancelled: " + path, ECANCELED);
                return false;
            }
            if (phase != Phase::Queued) {
                error = IoError::retryable("local commit is already in progress: " + path, EBUSY);
                return false;
            }
            phase = Phase::Committing;
            handle = fd;
        }

        bool ok = true;
        try {
            while ((operations.fsync ? operations.fsync(handle) : ::fsync(handle)) != 0) {
                if (errno == EINTR) continue;
                error = IoError::from_errno("fsync failed", path);
                ok = false;
                break;
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex);
            if (abort_requested) abort_locked();
            else phase = Phase::Failed;
            throw;
        }

        std::lock_guard<std::mutex> lock(mutex);
        if (abort_requested) {
            abort_locked();
            error = IoError::retryable("local commit cancelled: " + path, ECANCELED);
            return false;
        }
        if (ok) {
            phase = Phase::Persisted;
            close_locked();
            release_uncommitted_locked();
        } else {
            phase = Phase::Failed;
        }
        return ok;
    }
    bool finish_commit(bool ok, IoError& error) {
        std::lock_guard<std::mutex> lock(mutex);
        if (phase == Phase::Queued) phase = Phase::Open; // admission was rejected
        if (phase == Phase::Persisted) {
            if (ok && !abort_requested) phase = Phase::Committed;
            else abort_locked();
        }
        if (phase == Phase::Aborted) {
            error = IoError::retryable("local commit cancelled: " + path, ECANCELED);
            return false;
        }
        return ok;
    }
    void release_rejected_commit() noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        if (phase == Phase::Queued) phase = Phase::Open;
    }
    void fail_completion() noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        if (phase == Phase::Queued) phase = Phase::Open;
        else if (phase == Phase::Persisted) abort_locked();
    }

    std::mutex mutex;
    int fd;
    const std::string path;
    const std::shared_ptr<LocalStorageRuntime> runtime;
    const Operations operations;
    Phase phase = Phase::Open;
    bool abort_requested = false;
    std::uint64_t uncommitted_bytes = 0;
};

std::unique_ptr<LocalFileWriteStream> LocalFileWriteStream::open(
    const std::string& path, IoError& error, std::shared_ptr<LocalStorageRuntime> runtime,
    Operations operations) {
    if (path.empty()) {
        error = IoError::permanent("storage key is empty");
        return nullptr;
    }
    try {
        const auto parent = std::filesystem::path(path).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
    } catch (const std::exception& e) {
        error = IoError::retryable("failed to create parent directory for " + path + ": " + e.what());
        return nullptr;
    }
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        error = IoError::from_errno("open failed", path);
        return nullptr;
    }
    std::shared_ptr<State> state;
    try {
        state = std::make_shared<State>(fd, path, std::move(runtime), std::move(operations));
    } catch (...) {
        ::close(fd);
        ::unlink(path.c_str());
        throw;
    }
    return std::unique_ptr<LocalFileWriteStream>(new LocalFileWriteStream(std::move(state)));
}

LocalFileWriteStream::~LocalFileWriteStream() { abort(); }

bool LocalFileWriteStream::write_at(std::uint64_t offset, const char* data,
                                    std::size_t size, IoError& error) {
    auto state = state_;
    int fd;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->phase != State::Phase::Open) {
            const bool busy = state->phase == State::Phase::Writing ||
                              state->phase == State::Phase::Queued ||
                              state->phase == State::Phase::Committing;
            error = busy ? IoError::retryable("local stream has an operation in progress", EBUSY)
                         : IoError::permanent("write stream is closed: " + state->path);
            return false;
        }
        if (!data || size == 0) return true;
        const auto max_offset = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
        if (offset > max_offset || size > max_offset - offset) {
            error = IoError::permanent("local write exceeds file offset range", EFBIG);
            return false;
        }
        state->phase = State::Phase::Writing;
        fd = state->fd;
    }
    auto guard = state->runtime ? state->runtime->begin_write(size) : LocalStorageRuntime::WriteGuard{};
    std::size_t done = 0;
    bool ok = true;
    try {
        while (done < size) {
            const auto n = state->operations.pwrite
                ? state->operations.pwrite(fd, data + done, size - done, offset + done)
                : ::pwrite(fd, data + done, size - done, static_cast<off_t>(offset + done));
            if (n < 0) {
                if (errno == EINTR) continue;
                error = IoError::from_errno("pwrite failed", state->path);
                ok = false;
                break;
            }
            if (n == 0) {
                error = IoError::retryable("pwrite wrote 0 bytes for " + state->path);
                ok = false;
                break;
            }
            done += static_cast<std::size_t>(n);
        }
    } catch (...) {
        state->finish_write(done, false);
        guard.finish(false, done);
        throw;
    }
    state->finish_write(done, ok);
    guard.finish(ok, done);
    return ok;
}

bool LocalFileWriteStream::commit(IoError& error) {
    auto state = state_;
    std::uint64_t bytes;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->phase == State::Phase::Committed) return true;
        if (state->phase != State::Phase::Open) {
            error = IoError::retryable("local stream is not ready to commit: " + state->path, EBUSY);
            return false;
        }
        state->phase = State::Phase::Queued;
        bytes = state->uncommitted_bytes;
    }
    bool ok;
    try {
        if (state->runtime) {
            ok = state->runtime->commit_sync(bytes, [state](IoError& err) { return state->persist(err); }, error);
        } else {
            ok = state->persist(error);
        }
    } catch (...) {
        state->release_rejected_commit();
        throw;
    }
    return state->finish_commit(ok, error);
}

void LocalFileWriteStream::commit_async(CommitCallback cb) {
    commit_async_on({}, std::move(cb));
}

void LocalFileWriteStream::commit_async_on(CompletionDispatcher dispatcher, CommitCallback cb) {
    auto state = state_;
    std::uint64_t bytes = 0;
    IoError error;
    bool already_committed = false;
    bool ready = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        already_committed = state->phase == State::Phase::Committed;
        if (state->phase == State::Phase::Open) {
            state->phase = State::Phase::Queued;
            bytes = state->uncommitted_bytes;
            ready = true;
        } else if (!already_committed) {
            error = IoError::retryable("local stream is not ready to commit: " + state->path, EBUSY);
        }
    }
    if (!ready) {
        // No I/O is pending. Keeping this notification inline also makes repeated
        // commits/rejections usable while the completion executor is shutting down.
        if (cb) cb(already_committed, error);
        return;
    }
    auto complete = [state, cb = std::move(cb)](bool ok, const IoError& err) mutable {
        IoError result;
        try {
            result = err;
            ok = state->finish_commit(ok, result);
        } catch (...) {
            state->fail_completion();
            result.kind = IoError::Kind::Retryable;
            result.code = ENOMEM;
            result.message.clear();
            ok = false;
        }
        if (cb) cb(ok, result);
    };
    try {
        if (state->runtime) {
            state->runtime->submit_commit(bytes,
                [state](IoError& err) { return state->persist(err); },
                std::move(complete), std::move(dispatcher));
        } else {
            // Without a runtime the syscall remains inline, but an explicit
            // completion dispatcher still owns publication of the result.
            const bool ok = state->persist(error);
            dispatch_completion(std::move(dispatcher), std::move(complete), ok, std::move(error));
        }
    } catch (...) {
        state->release_rejected_commit();
        throw;
    }
}

void LocalFileWriteStream::abort() noexcept { state_->abort(); }

} // namespace pr
