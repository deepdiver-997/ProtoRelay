#include "mail_system/back/thread_pool/io_thread_pool.h"

namespace mail_system {

IOThreadPool::IOThreadPool(size_t thread_count)
    : m_thread_count(thread_count), m_io_contexts(thread_count, std::make_shared<boost::asio::io_context>()),
      m_running(false) {
}

IOThreadPool::~IOThreadPool() {
    stop(true);
}

void IOThreadPool::start() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_running) return;

    m_running = true;
    std::cout << "Starting IOThreadPool..." << std::endl;

    m_work_guards.reserve(m_thread_count);
    for(int i = 0;i < m_thread_count; ++i)
        m_work_guards.emplace_back(boost::asio::make_work_guard(
            m_io_contexts[i]->get_executor()));

    m_threads.reserve(m_thread_count);
    for (size_t i = 0; i < m_thread_count; ++i) {
        m_threads.emplace_back([this, i]() {
            try {
                m_io_contexts[i]->run();
            } catch (const std::exception& e) {
                std::cerr << "Exception in IO thread: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "Unknown exception in IO thread" << std::endl;
            }
        });
    }
}

void IOThreadPool::stop(bool wait_for_tasks) {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_running) return;

    m_running = false;
    std::cout << "Stopping IOThreadPool..." << std::endl;

    for(auto& m_work_guard : m_work_guards) {
        if (m_work_guard.owns_work()) {
            m_work_guard.reset();
        }
    }

    if (wait_for_tasks) {
        for (auto& io_context : m_io_contexts) {
            io_context->run();
        }
    }

    for (auto& thread : m_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

size_t IOThreadPool::thread_count() const {
    return m_thread_count;
}

bool IOThreadPool::is_running() const {
    return m_running.load();
}

}