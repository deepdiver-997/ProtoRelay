#ifndef PR_FRAMEWORK_BOOST_THREAD_POOL_H
#define PR_FRAMEWORK_BOOST_THREAD_POOL_H

#include "thread_pool_base.h"
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/bind.hpp>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>

namespace pr {

/**
 * @brief 基于Boost的线程池实现
 * 
 * 这个类使用Boost.Asio的thread_pool来实现线程池功能，
 * 适用于处理CPU密集型或阻塞性任务。
 */
class BoostThreadPool : public ThreadPoolBase {
public:
    /**
     * @brief 构造函数
     * 
     * @param thread_count 线程数量，默认为系统硬件并发数
     */
    explicit BoostThreadPool(size_t thread_count = std::thread::hardware_concurrency());

    /**
     * @brief 析构函数
     * 
     * 确保线程池在销毁前停止
     */
    ~BoostThreadPool();

    /**
     * @brief 启动线程池
     */
        void start() override; // Prepare to add override keyword

    /**
     * @brief 停止线程池
     * 
     * @param wait_for_tasks 是否等待所有任务完成
     */
        void stop(bool wait_for_tasks = true) override; // Prepare to add override keyword

    /**
     * @brief 获取线程池中的线程数量
     * 
     * @return size_t 线程数量
     */
        size_t thread_count() const override; // Prepare to add override keyword

    /**
     * @brief 检查线程池是否正在运行
     *
     * @return true 如果线程池正在运行
     * @return false 如果线程池已停止
     */
        bool is_running() const override; // Prepare to add override keyword

    /**
     * @brief 任务边界捕获到的异常累计数（post 路径）。
     *  submit 路径的异常存进 future、由 .get() 侧处理，不经此计数。
     */
    uint64_t exceptions_total() const {
        return m_exceptions_total.load(std::memory_order_relaxed);
    }

protected:
    /**
     * @brief 提交任务的实现（无返回值版本）
     *
     * 实现在 .cpp：任务边界兜异常（boost::asio::thread_pool 工作线程不捕获 handler
     * 异常，任务抛出穿出线程入口 = std::terminate 整进程），兜底要记日志，
     * 头文件不引日志头。
     */
    void post_impl(std::function<void()> f) override;

private:
    size_t m_thread_count;                          ///< 线程数量
    std::unique_ptr<boost::asio::thread_pool> m_pool; ///< Boost线程池
    std::atomic<bool> m_running;                                 ///< 线程池是否运行中
    std::mutex m_mutex;                             ///< 互斥锁，保护线程池状态
    std::atomic<uint64_t> m_exceptions_total{0};    ///< 任务边界捕获的异常累计（见 exceptions_total）
};

} // namespace pr

#endif // PR_FRAMEWORK_BOOST_THREAD_POOL_H