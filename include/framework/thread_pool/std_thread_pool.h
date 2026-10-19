#ifndef PR_FRAMEWORK_STD_THREAD_POOL_H
#define PR_FRAMEWORK_STD_THREAD_POOL_H
#include "framework/thread_pool/thread_pool_base.h"
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace pr {
// Lifecycle operations must be called from outside the pool's own workers.
// stop(false) discards queued tasks, then joins tasks already executing; it
// cannot interrupt arbitrary user code. Discarded submit futures become broken.
class StdThreadPool final : public ThreadPoolBase {
public:
    explicit StdThreadPool(size_t thread_count = std::thread::hardware_concurrency());
    ~StdThreadPool() override;
    void start() override;
    void stop(bool wait_for_tasks = true) override;
    size_t thread_count() const override { return thread_count_; }
    bool is_running() const override { return running_.load(std::memory_order_acquire); }
    uint64_t exceptions_total() const { return exceptions_.load(std::memory_order_relaxed); }
protected:
    void post_impl(std::function<void()> task) override;
private:
    void run();
    const size_t thread_count_;
    std::mutex lifecycle_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::queue<std::function<void()>> tasks_;
    std::vector<std::thread> threads_;
    std::atomic<bool> running_{false};
    bool stopping_ = false;
    std::atomic<uint64_t> exceptions_{0};
};
} // namespace pr
#endif
