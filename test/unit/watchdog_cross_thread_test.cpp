// Watchdog 跨线程续跑压力 —— 真实 io 线程 + 真实 timer，TSan 插桩目标。
//
// 覆盖 docs/local/TODO.md "watchdog 恶意超时专项测试" 的竞争面：SMTP/IMAP/POP3
// 的 DB/worker 回调续跑（drain_buffered_commands → handle_read → rearm）发生在
// 非 io 线程，与 io 线程的 timer fire / close 并发。SessionBase::rearm 的契约是
// "一律 post 收敛到连接 executor"；本测试用真实线程持续从外部线程 rearm、真实
// 短窗口让 watchdog 到期、最后在 rearm 尚在途时 close —— 判定交给 TSan
//（test/README.md：ENABLE_TSAN=ON，grep "WARNING: ThreadSanitizer"）。
// 无 TSan 的常规 ctest 运行只验证功能时序收敛（最终回收 + 无泄漏），约 1s。
#undef NDEBUG
#include "watchdog_wire.h"
#include "framework/session_base.tpp"
#include "mail_system/back/mailServer/fsm/pop3/traditional_pop3_fsm.tpp"
#include "mail_system/back/mailServer/session/pop3_session.tpp"
#include "mail_system/back/router/static_shard_router.h"
#include "mail_system/back/common/logger.h"
#include "framework/thread_pool/io_thread_pool.h"
#include "framework/thread_pool/worker_thread_pool.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <memory>
#include <thread>

namespace {

using mail_system::test::WatchdogWire;
using Router = mail_system::router::StaticShardRouter;

template <typename T>
bool wait_for(const T& predicate, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

} // namespace

int main() {
    mail_system::Logger::get_instance().set_handler({});

    auto io = std::make_shared<pr::IOThreadPool>(1);
    auto worker = std::make_shared<pr::WorkerThreadPool>(1);
    io->start();
    worker->start();

    auto router = std::make_shared<Router>(std::vector<std::pair<std::string, int>>{}, 0,
        std::vector<std::shared_ptr<pr::DBPool>>{},
        std::vector<std::shared_ptr<pr::storage::IStorageProvider>>{});
    auto fsm = std::make_shared<mail_system::TraditionalPop3Fsm<WatchdogWire>>(worker, worker, router);

    auto conn_u = std::make_unique<WatchdogWire>(io->next_executor());
    auto* wire = conn_u.get();
    auto session = std::make_shared<mail_system::Pop3Session<WatchdogWire>>(
        nullptr, std::move(conn_u), fsm);
    mail_system::Pop3Session<WatchdogWire>::start(session);

    // banner 就绪 = io 线程把 rearm/handshake/CONNECT 链跑完
    assert((wait_for([&] { return wire->written().find("+OK") == 0; },
                     std::chrono::milliseconds(2000))));

    // 外部线程模拟 worker 续跑路径：持续 rearm（窗口 30~50ms 交替）
    std::atomic<bool> stop_rearm{false};
    std::thread foreign([&] {
        int i = 0;
        while (!stop_rearm.load(std::memory_order_relaxed) && !session->is_closed()) {
            session->rearm(std::chrono::milliseconds(30 + (i++ % 3) * 10));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    stop_rearm.store(true);
    foreign.join();

    // 最后一次 rearm 的窗口 ≤50ms；不再续命 → watchdog 到期 → cancel 挂起读 → 回收
    assert((wait_for([&] { return session->is_closed(); },
                     std::chrono::milliseconds(3000))));
    assert(wire->cancel_count() >= 1);
    assert(wire->written().find("+OK") == 0);   // 只发过 banner，未被误写

    // close → disarm 断 self 环；读完成侧已无强引用可滞留
    std::weak_ptr<mail_system::Pop3Session<WatchdogWire>> weak = session;
    session.reset();
    assert((wait_for([&] { return weak.expired(); },
                     std::chrono::milliseconds(3000))));

    worker->stop();
    io->stop();
    return 0;
}
