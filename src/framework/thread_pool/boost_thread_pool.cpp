#include "framework/thread_pool/boost_thread_pool.h"
#include "framework/log/log_macros.h"

namespace mail_system {

BoostThreadPool::BoostThreadPool(size_t thread_count)
    : m_thread_count(thread_count), m_running(false) {
}

BoostThreadPool::~BoostThreadPool() {
    stop(true);
}

void BoostThreadPool::start() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_running) {
        m_running = true;
        LOG_THREAD_POOL_INFO("Starting BoostThreadPool...");
        m_pool = std::make_unique<boost::asio::thread_pool>(m_thread_count);
    }
}

void BoostThreadPool::stop(bool wait_for_tasks) {
    std::unique_ptr<boost::asio::thread_pool> pool;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running) return;
        m_running = false;
        pool = std::move(m_pool);
    }

    if (pool) {
        if (wait_for_tasks) {
            pool->join();
        }
        pool->stop();
        LOG_THREAD_POOL_INFO("Stopped BoostThreadPool");
        pool->join();
    }
}

size_t BoostThreadPool::thread_count() const {
    return m_thread_count;
}

bool BoostThreadPool::is_running() const {
    return m_running;
}

void BoostThreadPool::post_impl(std::function<void()> f) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running) {
            throw std::runtime_error("Thread pool is not running");
        }
        auto* pool = m_pool.get();
        // 任务边界兜异常：boost::asio::thread_pool 的工作线程不捕获 handler 抛出的
        // 异常，异常穿出线程入口 = std::terminate 整进程（2026-09 认证路径 std::stoi
        // 遇 DB 脏数据即此形状：一行脏数据炸掉全部在线连接）。兜住 = 失败只归因于
        // 这一单任务。
        // submit() 语义不变：packaged_task 自己把异常存进 future（(*task)() 不向外
        // 抛），.get() 处仍按原样 rethrow 给调用方的失败路径，不经此计数。
        auto task = std::make_shared<std::function<void()>>(std::move(f));
        boost::asio::post(*pool, [this, task]() mutable {
            try {
                (*task)();
            } catch (const std::exception& e) {
                const auto n = m_exceptions_total.fetch_add(1, std::memory_order_relaxed) + 1;
                LOG_THREAD_POOL_ERROR("Worker task threw (total={}): {}", n, e.what());
            } catch (...) {
                const auto n = m_exceptions_total.fetch_add(1, std::memory_order_relaxed) + 1;
                LOG_THREAD_POOL_ERROR("Worker task threw non-standard exception (total={})", n);
            }
        });
    }
}

}