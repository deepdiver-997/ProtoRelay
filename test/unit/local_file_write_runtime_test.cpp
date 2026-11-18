#ifdef NDEBUG
#undef NDEBUG
#endif
#include "framework/storage/local_file_storage_provider.h"
#include "framework/storage/local_file_write_stream.h"
#include "framework/storage/local_storage_runtime.h"
#include "mail_system/back/storage/mail_body_writer.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <fcntl.h>
#include <unistd.h>

using namespace std::chrono_literals;
using pr::IoError;
using pr::LocalFileWriteStream;
using pr::LocalStorageRuntime;

namespace {
struct Directory {
    Directory() {
        char pattern[] = "/tmp/protorelay_local_runtime_XXXXXX";
        const auto p = ::mkdtemp(pattern);
        assert(p);
        path = p;
    }
    ~Directory() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    std::string file(const std::string& name) const { return path + "/" + name; }
    std::string path;
};

struct Gate {
    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> ready = release.get_future().share();
    void wait() {
        entered.set_value();
        assert(ready.wait_for(5s) == std::future_status::ready);
    }
};

struct Dispatcher {
    void post(std::function<void()> task) {
        std::lock_guard<std::mutex> lock(mutex);
        tasks.push_back(std::move(task));
        cv.notify_all();
    }
    void run_one() {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex);
            assert(cv.wait_for(lock, 5s, [&] { return !tasks.empty(); }));
            task = std::move(tasks.front());
            tasks.pop_front();
        }
        task();
    }
    void wait_for_task() {
        std::unique_lock<std::mutex> lock(mutex);
        assert(cv.wait_for(lock, 5s, [&] { return !tasks.empty(); }));
    }
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void()>> tasks;
};

template <typename T> T get(std::future<T>& future) {
    assert(future.wait_for(5s) == std::future_status::ready);
    return future.get();
}

std::unique_ptr<LocalFileWriteStream> written(
    const std::string& path, const std::shared_ptr<LocalStorageRuntime>& runtime,
    LocalFileWriteStream::Operations operations = {}) {
    IoError error;
    auto stream = LocalFileWriteStream::open(path, error, runtime, std::move(operations));
    assert(stream && stream->write_at(0, "abc", 3, error));
    return stream;
}

void legacy_and_shared_accounting() {
    Directory dir;
    IoError error;
    auto legacy = written(dir.file("legacy"), {});
    bool called = false;
    legacy->commit_async([&](bool ok, const IoError&) { assert(ok); called = true; });
    assert(called);
    legacy.reset();
    assert(std::filesystem::file_size(dir.file("legacy")) == 3);

    auto runtime = std::make_shared<LocalStorageRuntime>();
    pr::storage::LocalFileStorageProvider first({}, runtime), second({}, runtime);
    assert(first.local_storage_runtime() == second.local_storage_runtime());
    auto a = first.open_write(dir.file("a"), error);
    auto b = second.open_write(dir.file("b"), error);
    assert(a && b);
    assert(a->write_at(0, "one", 3, error));
    assert(b->write_at(0, "two", 3, error));
    auto stats = runtime->snapshot();
    assert(stats.writes_total == 2 && stats.write_bytes_written == 6);
    assert(stats.write_elapsed_count == 2 && stats.write_elapsed_total >= stats.write_elapsed_max);
    assert(stats.writes_in_flight == 0 && stats.write_bytes_in_flight == 0);
    assert(stats.uncommitted_bytes == 6);
    assert(a->commit(error));
    assert(runtime->snapshot().uncommitted_bytes == 3);
    b->abort();
    assert(runtime->snapshot().uncommitted_bytes == 0);
    assert(!std::filesystem::exists(dir.file("b")));
    runtime->shutdown();
}

void running_abort_does_not_close_fd() {
    Directory dir;
    auto runtime = std::make_shared<LocalStorageRuntime>();
    Gate gate;
    auto entered = gate.entered.get_future();
    std::atomic<int> handle{-1};
    LocalFileWriteStream::Operations ops;
    ops.fsync = [&](int fd) {
        handle = fd;
        gate.wait();
        return ::fsync(fd);
    };
    auto stream = written(dir.file("running"), runtime, ops);
    Dispatcher dispatcher;
    std::promise<bool> done;
    auto result = done.get_future();
    const auto owner = std::this_thread::get_id();
    stream->commit_async_on([&](auto cb) { dispatcher.post(std::move(cb)); },
        [&](bool ok, const IoError& error) {
            assert(std::this_thread::get_id() == owner);
            assert(!ok && error.retryable());
            done.set_value(ok);
        });
    get(entered);
    assert(runtime->snapshot().commits_running == 1);
    // Owner-executor work can progress while a dedicated worker waits in fsync.
    bool heartbeat = false;
    dispatcher.post([&] { heartbeat = true; });
    dispatcher.run_one();
    assert(heartbeat);
    stream->abort();
    assert(::fcntl(handle, F_GETFD) >= 0);
    stream.reset();
    gate.release.set_value();
    dispatcher.run_one();
    assert(!get(result));
    runtime->drain();
    assert(!std::filesystem::exists(dir.file("running")));
    assert(runtime->snapshot().uncommitted_bytes == 0);
    assert(runtime->snapshot().commits_outstanding == 0);
}

void queued_abort_skips_fsync() {
    Directory dir;
    pr::LocalStorageRuntimeConfig config;
    config.commit_threads = 1;
    auto runtime = std::make_shared<LocalStorageRuntime>(config);
    Gate gate;
    auto entered = gate.entered.get_future();
    assert(runtime->submit_commit(0, [&](IoError&) { gate.wait(); return true; }, {}));
    get(entered);
    std::atomic<int> syncs{0};
    LocalFileWriteStream::Operations ops;
    ops.fsync = [&](int fd) { ++syncs; return ::fsync(fd); };
    auto stream = written(dir.file("queued"), runtime, ops);
    std::promise<bool> done;
    auto result = done.get_future();
    stream->commit_async([&](bool ok, const IoError&) { done.set_value(ok); });
    assert(runtime->snapshot().commits_queued == 1);
    stream->abort();
    stream.reset();
    assert(!std::filesystem::exists(dir.file("queued")));
    gate.release.set_value();
    assert(!get(result));
    runtime->drain();
    assert(syncs == 0 && runtime->snapshot().uncommitted_bytes == 0);
}

void abort_before_completion_publication() {
    Directory dir;
    auto runtime = std::make_shared<LocalStorageRuntime>();
    auto stream = written(dir.file("publication"), runtime);
    Dispatcher dispatcher;
    bool called = false;
    stream->commit_async_on([&](auto cb) { dispatcher.post(std::move(cb)); },
        [&](bool ok, const IoError&) { assert(!ok); called = true; });
    dispatcher.wait_for_task();
    assert(runtime->snapshot().commits_running == 0);
    assert(runtime->snapshot().commits_completion_pending == 1);
    stream->abort();
    assert(!std::filesystem::exists(dir.file("publication")));
    dispatcher.run_one();
    runtime->drain();
    assert(called && runtime->snapshot().commits_outstanding == 0);
}

void rejection_does_not_execute_and_can_retry() {
    Directory dir;
    pr::LocalStorageRuntimeConfig config;
    config.max_pending_commits = 1;
    auto runtime = std::make_shared<LocalStorageRuntime>(config);
    auto first = written(dir.file("first"), runtime);
    Dispatcher dispatcher;
    first->commit_async_on([&](auto cb) { dispatcher.post(std::move(cb)); },
        [](bool ok, const IoError&) { assert(ok); });
    dispatcher.wait_for_task();
    std::atomic<int> syncs{0};
    LocalFileWriteStream::Operations ops;
    ops.fsync = [&](int fd) { ++syncs; return ::fsync(fd); };
    auto second = written(dir.file("second"), runtime, ops);
    bool rejected = false;
    second->commit_async([&](bool ok, const IoError& error) {
        assert(!ok && error.retryable());
        rejected = true;
    });
    assert(rejected && syncs == 0);
    assert(runtime->snapshot().uncommitted_bytes == 3);
    dispatcher.run_one();
    runtime->drain();
    IoError error;
    assert(second->commit(error) && syncs == 1);
    assert(runtime->snapshot().uncommitted_bytes == 0);
}

void short_write_and_eintr() {
    Directory dir;
    auto runtime = std::make_shared<LocalStorageRuntime>();
    int calls = 0;
    LocalFileWriteStream::Operations ops;
    ops.pwrite = [&](int fd, const char* data, std::size_t size, std::uint64_t offset) -> std::ptrdiff_t {
        if (++calls == 1) { errno = EINTR; return -1; }
        return ::pwrite(fd, data, std::min<std::size_t>(size, 2), static_cast<off_t>(offset));
    };
    auto stream = written(dir.file("short"), runtime, ops);
    IoError error;
    assert(calls == 3 && stream->commit(error));
    assert(runtime->snapshot().writes_total == 1);
    assert(runtime->snapshot().write_bytes_written == 3);
    std::ifstream file(dir.file("short"), std::ios::binary);
    std::string body{std::istreambuf_iterator<char>(file), {}};
    assert(body == "abc");
}

void partial_write_failure_accounted() {
    Directory dir;
    auto runtime = std::make_shared<LocalStorageRuntime>();
    int calls = 0;
    LocalFileWriteStream::Operations ops;
    ops.pwrite = [&](int fd, const char* data, std::size_t, std::uint64_t offset) -> std::ptrdiff_t {
        if (++calls == 1) return ::pwrite(fd, data, 1, static_cast<off_t>(offset));
        errno = ENOSPC;
        return -1;
    };
    IoError error;
    auto stream = LocalFileWriteStream::open(dir.file("partial"), error, runtime, ops);
    assert(stream && !stream->write_at(0, "abc", 3, error));
    assert(error.retryable() && error.code == ENOSPC);
    auto stats = runtime->snapshot();
    assert(stats.writes_failed == 1 && stats.write_bytes_failed == 1);
    assert(stats.write_bytes_in_flight == 0 && stats.uncommitted_bytes == 1);
    stream->abort();
    assert(runtime->snapshot().uncommitted_bytes == 0);
    assert(!std::filesystem::exists(dir.file("partial")));
}

void fsync_error_and_eintr() {
    Directory dir;
    auto runtime = std::make_shared<LocalStorageRuntime>();
    std::atomic<int> calls{0};
    LocalFileWriteStream::Operations ops;
    ops.fsync = [&](int) {
        errno = ++calls == 1 ? EINTR : ENOSPC;
        return -1;
    };
    auto stream = written(dir.file("sync-error"), runtime, ops);
    std::promise<bool> done;
    auto result = done.get_future();
    stream->commit_async([&](bool ok, const IoError& error) {
        assert(!ok && error.retryable() && error.code == ENOSPC);
        done.set_value(ok);
    });
    assert(!get(result));
    runtime->drain();
    assert(calls == 2 && runtime->snapshot().commits_failed == 1);
    assert(runtime->snapshot().uncommitted_bytes == 3);
    stream.reset();
    assert(runtime->snapshot().uncommitted_bytes == 0);
}

void dispatcher_failure_and_late_closure() {
    Directory dir;
    auto stream = written(dir.file("dispatch"), {});
    std::function<void()> late;
    int calls = 0;
    stream->commit_async_on([&](auto cb) {
        late = std::move(cb);
        throw std::runtime_error("handoff failed");
    }, [&](bool ok, const IoError& error) {
        assert(!ok && error.retryable());
        ++calls;
    });
    assert(calls == 1 && !std::filesystem::exists(dir.file("dispatch")));
    late();
    assert(calls == 1);
}

void writer_pinned_through_dispatch(bool cancel) {
    Directory dir;
    auto runtime = std::make_shared<LocalStorageRuntime>();
    IoError error;
    auto stream = LocalFileWriteStream::open(dir.file("writer"), error, runtime);
    auto writer = std::make_shared<mail_system::storage::MailBodyWriter>(std::move(stream));
    assert(writer->write("abc", 3, error));
    Dispatcher dispatcher;
    std::weak_ptr<mail_system::storage::MailBodyWriter> weak = writer;
    bool called = false;
    writer->commit_async([keep = writer, &called, cancel](bool ok, const IoError&) {
        assert(ok != cancel && keep->committed() == ok);
        called = true;
    }, [&](auto cb) { dispatcher.post(std::move(cb)); });
    dispatcher.wait_for_task();
    if (cancel) writer->abort();
    writer.reset();
    assert(!weak.expired());
    dispatcher.run_one();
    runtime->drain();
    assert(called && weak.expired());
    assert(std::filesystem::exists(dir.file("writer")) != cancel);
}
} // namespace

int main() {
    legacy_and_shared_accounting();
    running_abort_does_not_close_fd();
    queued_abort_skips_fsync();
    abort_before_completion_publication();
    rejection_does_not_execute_and_can_retry();
    short_write_and_eintr();
    partial_write_failure_accounted();
    fsync_error_and_eintr();
    dispatcher_failure_and_late_closure();
    writer_pinned_through_dispatch(false);
    writer_pinned_through_dispatch(true);
    std::cout << "local_file_write_runtime_test: 11 cases passed\n";
}
