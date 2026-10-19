#include "framework/thread_pool/std_thread_pool.h"
#include "framework/log/log_macros.h"
#include <stdexcept>

namespace pr {
namespace {
thread_local const StdThreadPool* current_worker_pool = nullptr;
}
StdThreadPool::StdThreadPool(size_t count) : thread_count_(count) {
    if (!count) throw std::invalid_argument("StdThreadPool: thread_count must be > 0");
}

StdThreadPool::~StdThreadPool() { stop(true); }

void StdThreadPool::start() {
    if (current_worker_pool == this)
        throw std::logic_error("StdThreadPool: lifecycle operations require an external thread");
    std::lock_guard<std::mutex> lifecycle(lifecycle_);
    if (is_running()) return;
    std::unique_lock<std::mutex> lock(mutex_);
    stopping_ = false;
    threads_.reserve(thread_count_);
    try {
        for (size_t i = 0; i < thread_count_; ++i) threads_.emplace_back([this] { run(); });
        running_.store(true, std::memory_order_release);
    } catch (...) {
        stopping_ = true;
        lock.unlock();
        ready_.notify_all();
        for (auto& thread : threads_) thread.join();
        threads_.clear();
        throw;
    }
}

void StdThreadPool::stop(bool wait_for_tasks) {
    if (current_worker_pool == this)
        throw std::logic_error("StdThreadPool: cannot join its own worker");
    std::lock_guard<std::mutex> lifecycle(lifecycle_);
    std::queue<std::function<void()>> discarded;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_.exchange(false, std::memory_order_acq_rel)) return;
        stopping_ = true;
        if (!wait_for_tasks) tasks_.swap(discarded);
    }
    ready_.notify_all();
    // Destroy queued packaged_tasks outside mutex_, delivering broken_promise.
    discarded = {};
    for (auto& thread : threads_) thread.join();
    threads_.clear();
}

void StdThreadPool::post_impl(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!is_running()) throw std::runtime_error("StdThreadPool is not running");
        tasks_.push(std::move(task));
    }
    ready_.notify_one();
}

void StdThreadPool::run() {
    current_worker_pool = this;
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (tasks_.empty()) break;
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        try { task(); }
        catch (const std::exception& error) {
            const auto count = exceptions_.fetch_add(1, std::memory_order_relaxed) + 1;
            LOG_THREAD_POOL_ERROR("Worker task threw (total={}): {}", count, error.what());
        } catch (...) {
            const auto count = exceptions_.fetch_add(1, std::memory_order_relaxed) + 1;
            LOG_THREAD_POOL_ERROR("Worker task threw non-standard exception (total={})", count);
        }
    }
    current_worker_pool = nullptr;
}
} // namespace pr
