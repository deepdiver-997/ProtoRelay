// 线程池单元测试：pr::WorkerThreadPool（submit/post 真实执行）与
// pr::IOThreadPool（get_io_context 生产路径）。
//
// ⚠️ pr::IOThreadPool::submit()/post() 是死 API：post_impl 是空实现（注释掉了），
//    提交的任务永远不会执行、future 永不完成 —— 若调用 submit 并阻塞 get 会挂死。
//    生产代码从不走这条路（accept/session 直接用 asio 在 get_io_context 上 post），
//    因此这里只测 get_io_context + asio::post 的真实用法，不调死 API。
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

#include <boost/asio/post.hpp>

#include "framework/thread_pool/io_thread_pool.h"
#include "framework/thread_pool/worker_thread_pool.h"

namespace {

using pr::WorkerThreadPool;
using pr::IOThreadPool;

int g_pass = 0, g_fail = 0;
void expect_true(bool c, const char* what) {
    if (c) { g_pass++; }
    else { g_fail++; std::printf("  FAIL %s\n", what); }
}

template <typename F>
bool wait_until(F f, int timeout_ms = 3000) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return f();
}

} // namespace

int main() {
    std::printf("thread_pool_test\n");

    // ── pr::WorkerThreadPool ──────────────────────────────────────────
    {
        pr::WorkerThreadPool pool(2);
        expect_true(!pool.is_running(), "worker pool not running before start");
        pool.start();
        expect_true(pool.is_running(), "worker pool running after start");
        expect_true(pool.thread_count() == 2, "worker pool thread count");

        // submit：future 拿到返回值
        auto fut = pool.submit([](int a, int b) { return a + b; }, 2, 3);
        expect_true(fut.get() == 5, "worker submit returns value");

        // post：异步执行 + 原子计数
        std::atomic<int> counter{0};
        for (int i = 0; i < 100; i++) pool.post([&counter] { counter++; });
        expect_true(wait_until([&] { return counter.load() == 100; }),
                    "worker post executes all tasks");

        // 重复 start 幂等
        pool.start();
        expect_true(pool.is_running(), "worker double start idempotent");

        // stop(wait=true)：排空任务后再返回
        std::atomic<bool> drained{false};
        pool.post([&drained] {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            drained = true;
        });
        pool.stop(true);
        expect_true(drained.load(), "worker stop(true) drains pending tasks");
        expect_true(!pool.is_running(), "worker pool stopped");
    }

    // stop 后 submit 抛异常
    {
        pr::WorkerThreadPool pool(1);
        pool.start();
        pool.stop();
        bool threw = false;
        try {
            auto f = pool.submit([] { return 1; });
            (void)f;
        } catch (const std::runtime_error&) {
            threw = true;
        }
        expect_true(threw, "worker submit after stop throws runtime_error");
    }

    // 未 start 直接 stop 安全
    {
        pr::WorkerThreadPool pool(2);
        pool.stop();
        expect_true(true, "worker stop before start is safe");
    }

    // ── pr::IOThreadPool ────────────────────────────────────────────
    {
        pr::IOThreadPool pool(2);
        expect_true(!pool.is_running(), "io pool not running before start");
        pool.start();
        expect_true(pool.is_running(), "io pool running after start");
        expect_true(pool.thread_count() == 2, "io pool thread count");

        // Server maintenance can obtain a scheduler through the neutral
        // provider contract without downcasting to IOThreadPool/io_context.
        pr::net::IExecutorProvider& provider = pool;
        auto executor = provider.next_executor();
        std::atomic<bool> scheduled{false};
        executor->post([&] { scheduled = true; });
        expect_true(wait_until([&] { return scheduled.load(); }), "io runtime provides a working abstract executor");

        // 生产路径：get_io_context + asio::post → 工作线程执行
        std::atomic<bool> ran{false};
        auto& ctx = pool.get_io_context();
        boost::asio::post(ctx, [&ran] { ran = true; });
        expect_true(wait_until([&] { return ran.load(); }),
                    "io pool executes asio::post on its context");

        // 重复 start 幂等
        pool.start();
        expect_true(pool.is_running(), "io pool double start idempotent");

        pool.stop();
        expect_true(!pool.is_running(), "io pool stopped");
    }

    // ── 异常隔离（2026-10-17）────────────────────────────────────
    // 修复前：boost::asio::thread_pool 工作线程不接异常 → post 的任务抛出 =
    // std::terminate 整进程；io 线程 run() 被 catch 后退出不重进 → 该 context 上
    // 全部连接静默僵死。修复后：任务边界捕获 + 计数，线程/context 继续服务。
    {
        pr::WorkerThreadPool pool(1);
        pool.start();
        std::atomic<bool> after{false};
        pool.post([] { throw std::runtime_error("boom-post"); });
        pool.post([&after] { after = true; });
        expect_true(wait_until([&] { return after.load(); }),
                    "worker pool survives a throwing post task");
        expect_true(pool.exceptions_total() == 1, "worker post exception counted");

        // submit：packaged_task 把异常存进 future，池不受伤，.get() 按原语义 rethrow
        auto fut = pool.submit([]() -> int { throw std::runtime_error("boom-submit"); });
        bool rethrew = false;
        try { (void)fut.get(); } catch (const std::runtime_error&) { rethrew = true; }
        expect_true(rethrew, "submit exception reaches the future");
        expect_true(pool.exceptions_total() == 1, "submit exception not in post counter");
        std::atomic<bool> still{false};
        pool.post([&still] { still = true; });
        expect_true(wait_until([&] { return still.load(); }),
                    "worker pool alive after submit rethrow");
        pool.stop();
    }
    {
        pr::IOThreadPool pool(2);
        pool.start();
        auto& ctx = pool.get_io_context();
        std::atomic<bool> after{false};
        boost::asio::post(ctx, [] { throw std::runtime_error("boom-handler"); });
        boost::asio::post(ctx, [&after] { after = true; });
        expect_true(wait_until([&] { return after.load(); }),
                    "io context keeps serving after a handler threw");
        expect_true(pool.exceptions_total() == 1, "io handler exception counted");
        pool.stop();
        expect_true(!pool.is_running(), "io pool stops cleanly after exception");
    }

    // get_io_context 在 stop 后抛异常
    {
        pr::IOThreadPool pool(1);
        pool.start();
        pool.stop();
        bool threw = false;
        try {
            (void)pool.get_io_context();
        } catch (const std::runtime_error&) {
            threw = true;
        }
        expect_true(threw, "get_io_context after stop throws runtime_error");
    }

    // 未 start 直接 stop 安全
    {
        pr::IOThreadPool pool(2);
        pool.stop();
        expect_true(true, "io pool stop before start is safe");
    }

    std::printf("  pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
