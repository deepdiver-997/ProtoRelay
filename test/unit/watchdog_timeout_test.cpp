// Watchdog 恶意超时/闲置回收专项 —— 确定性单测（ManualExecutor + WatchdogWire）。
//
// 覆盖 docs/local/TODO.md "watchdog 恶意超时专项测试" 的单测可确定性部分：
//   1. 挂接即武装：start() 后一个窗口的 async_wait 已在 timer 上；到期 → cancel
//      挂起读（对端断电不 RST 的唯一观察点）→ 读完成见 close_requested_ → close
//      → disarm 断 timer↔self 引用环（泄漏守卫：weak_ptr 过期）。
//   2. slowloris 字节滴灌不成行 → 半行不进协议 → 无 rearm → 一个窗口即回收。
//   3. 慢客户端完整命令逐条到达 → 每条 rearm → 存活跨多个窗口（idle 语义、
//      无总预算）；停止进度后再满一窗 → 回收。
//   4. rearm 后设取代先设：只有最后一次 rearm 持有 pending wait。
//   5. rearm 从非 io 线程调用 → 只 post 不碰 timer（直接碰会被 ManualTimer
//      的 require_owner 断言拦截）；drain 后才生效。
//   6. 显式 close() → disarm cancel 一次 → timer↔self 环断开（weak 过期、
//      executor 队列排空）。
//   7. IMAP IDLE 撤看门狗（合法长空闲不误回收）+ DONE 重新 arm。
// 跨线程 rearm vs fire/close 的真实竞争由 watchdog_cross_thread_test
// （ENABLE_TSAN 插桩）覆盖；本文件全部用确定性 timer，无真实时延。
#undef NDEBUG
#include "manual_executor.h"
#include "watchdog_wire.h"
#include "framework/session_base.tpp"
#include "framework/server_config.h"
#include "mail_system/back/mailServer/fsm/pop3/traditional_pop3_fsm.tpp"
#include "mail_system/back/mailServer/session/pop3_session.tpp"
#include "mail_system/back/mailServer/fsm/imaps/traditional_imaps_fsm.tpp"
#include "mail_system/back/mailServer/session/imaps_session.tpp"
#include "mail_system/back/mailServer/idle_push_registry.h"
#include "mail_system/back/router/static_shard_router.h"
#include "mail_system/back/common/logger.h"
#include <cassert>
#include <chrono>
#include <memory>
#include <thread>
#include <utility>

using namespace mail_system;

namespace {

using mail_system::test::ManualExecutor;
using mail_system::test::WatchdogWire;

// ── 与 pop3_timer_boundary_test 同款的排队 Worker ────────────────
class QueuedPool : public pr::ThreadPoolBase {
public:
    void start() override { running = true; }
    void stop(bool = true) override { running = false; }
    bool is_running() const override { return running; }
    size_t thread_count() const override { return 1; }
    void drain() {
        while (!tasks.empty()) {
            auto task = std::move(tasks.front());
            tasks.pop_front();
            task();
        }
    }
    bool running = true;
    std::deque<std::function<void()>> tasks;

protected:
    void post_impl(std::function<void()> task) override { tasks.push_back(std::move(task)); }
};

using Router = mail_system::router::StaticShardRouter;

std::shared_ptr<Router> make_router() {
    return std::make_shared<Router>(std::vector<std::pair<std::string, int>>{}, 0,
        std::vector<std::shared_ptr<pr::DBPool>>{},
        std::vector<std::shared_ptr<pr::storage::IStorageProvider>>{});
}

// drain 到静默：io 队列与 worker 队列互相投递时交替排空（有界，防失控）。
void drain_quiet(const std::shared_ptr<ManualExecutor>& io,
                 const std::shared_ptr<QueuedPool>& worker) {
    for (int i = 0; i < 16; ++i) {
        io->drain();
        if (worker->tasks.empty()) return;
        worker->drain();
    }
    io->drain();
}

// ── POP3 会话支架：start() 挂看门狗 + banner + 挂起读，返回全部句柄 ──
struct Pop3Rig {
    std::shared_ptr<ManualExecutor> io = std::make_shared<ManualExecutor>();
    std::shared_ptr<QueuedPool> worker = std::make_shared<QueuedPool>();
    std::shared_ptr<TraditionalPop3Fsm<WatchdogWire>> fsm;
    std::unique_ptr<WatchdogWire> wire;
    std::shared_ptr<Pop3Session<WatchdogWire>> session;

    explicit Pop3Rig() {
        fsm = std::make_shared<TraditionalPop3Fsm<WatchdogWire>>(worker, worker, make_router());
        wire = std::make_unique<WatchdogWire>(io);
        session = std::make_shared<Pop3Session<WatchdogWire>>(nullptr, std::move(wire), fsm);
        wire = nullptr;  // unique_ptr 已被 move 进 session，防误用
        Pop3Session<WatchdogWire>::start(session);
        io->drain();
    }

    WatchdogWire& conn() {
        auto* raw = &session->get_connection();
        return *raw;
    }
    mail_system::test::ManualTimer* watchdog_timer() {
        assert(!io->timers.empty());
        return io->timers.front().get();
    }
    // 收尾：close + 排空 + 释放强引用后再排空；返回 weak 供过期断言。
    std::weak_ptr<Pop3Session<WatchdogWire>> finish() {
        std::weak_ptr<Pop3Session<WatchdogWire>> weak = session;
        session->set_trace_clean_close();
        session->close();
        io->drain();
        session.reset();
        io->drain();
        return weak;
    }
};

} // namespace

// ── 1. 挂接即武装 + 对端断电（不 RST）回收链 ─────────────────────
static void test_start_arms_and_silent_peer_recycled() {
    Pop3Rig rig;
    auto* timer = rig.watchdog_timer();
    assert(timer->waits == 1);
    assert(timer->delay == std::chrono::milliseconds(60000));   // 默认 read_timeout
    assert(rig.conn().written().find("+OK") == 0);              // banner 已上 wire
    assert(rig.conn().has_pending_read());                      // 空转挂起读在位

    timer->fire();                    // 模拟整个窗口无进度（断电对端不回数据）
    rig.io->drain();
    assert(rig.conn().cancel_count() == 1);   // watchdog → cancel 制造完成点
    assert(rig.session->close_requested());
    assert(rig.session->is_closed());         // 读完成(canceled) → 见标志 → close
    assert(timer->cancels == 1);              // close → disarm 断 self 环
    auto weak = rig.finish();
    assert(weak.expired());
    assert(rig.io->tasks.empty());
}

// ── 2. slowloris：字节滴灌不成行 → 一个窗口回收 ──────────────────
static void test_partial_line_no_rearm_slowloris_killed() {
    Pop3Rig rig;
    auto* timer = rig.watchdog_timer();
    // 半行不进协议（session_base 只派发完整行）→ handle_read 不跑 → 无 rearm。
    rig.conn().feed("NO");
    rig.io->drain();
    assert(timer->waits == 1);                // 窗口未被续命
    assert(!rig.session->is_closed());
    assert(rig.conn().cancel_count() == 0);
    timer->fire();
    rig.io->drain();
    assert(rig.session->is_closed());         // 滴灌挡不住闲置回收
}

// ── 3. 慢客户端：完整命令逐条到达 → idle rearm、无总预算 ─────────
static void test_slow_commands_idle_rearm_no_total_budget() {
    Pop3Rig rig;
    auto* timer = rig.watchdog_timer();
    // 每条命令重排一次窗口；连续 5 条全部"在窗口内"到达（feed 先于 fire）。
    // 累计 5 个完整窗口的存续时间，会话始终活着 → 回收只看 idle，无总预算。
    for (int i = 0; i < 5; ++i) {
        rig.conn().clear_written();
        rig.conn().feed("CAPA\r\n");
        drain_quiet(rig.io, rig.worker);
        assert(timer->waits == i + 2);        // 每条完整命令 rearm 一次
        assert(rig.conn().cancel_count() == 0);
        assert(!rig.session->close_requested());
        assert(!rig.session->is_closed());
        assert(rig.conn().has_pending_read()); // 响应写完成后回到挂起读
    }
    timer->fire();                            // 进度停止后再满一窗 → 回收
    rig.io->drain();
    assert(rig.session->is_closed());
    auto weak = rig.finish();
    assert(weak.expired());
}

// ── 4. rearm 后设取代先设：只有最后一次持有 pending wait ─────────
static void test_rearm_replaces_pending_window() {
    Pop3Rig rig;
    auto* timer = rig.watchdog_timer();
    rig.session->rearm(std::chrono::milliseconds(50));
    rig.io->drain();
    assert(timer->waits == 2 && timer->delay == std::chrono::milliseconds(50));

    // 同一批里连发两次：旧 wait 各自被 expires_after 以 canceled 收掉，
    // 只剩最后一次（30ms）挂 timer 上。
    rig.session->rearm(std::chrono::milliseconds(20));
    rig.session->rearm(std::chrono::milliseconds(30));
    rig.io->drain();
    assert(timer->waits == 4);
    assert(timer->delay == std::chrono::milliseconds(30));
    assert(timer->handler != nullptr);        // 恰一个 pending wait
    assert(!rig.session->close_requested());
    timer->fire();
    rig.io->drain();
    assert(rig.session->is_closed());
    auto weak = rig.finish();
    assert(weak.expired());
}

// ── 5. 跨线程 rearm 只 post 不碰 timer（require_owner 兜底）──────
static void test_rearm_from_foreign_thread_indirects_through_executor() {
    Pop3Rig rig;
    auto* timer = rig.watchdog_timer();
    std::thread foreign([&rig] {
        rig.session->rearm(std::chrono::milliseconds(1));   // worker 线程续跑路径
    });
    foreign.join();
    // drain 前 timer 不得有任何变化（ManualTimer 的 executor 亲和断言会拦直接访问）
    assert(timer->waits == 1);
    assert(timer->delay == std::chrono::milliseconds(60000));
    rig.io->drain();
    assert(timer->waits == 2);
    assert(timer->delay == std::chrono::milliseconds(1));
    auto weak = rig.finish();
    assert(weak.expired());
}

// ── 6. 显式 close：disarm 一次、环断开、队列排空 ─────────────────
static void test_close_cancels_watchdog_breaks_self_cycle() {
    Pop3Rig rig;
    auto* timer = rig.watchdog_timer();
    auto weak = rig.finish();
    assert(timer->cancels == 1);              // close → disarm → cancel
    assert(timer->handler == nullptr);        // async_wait 已被收掉
    assert(weak.expired());
    assert(rig.io->tasks.empty());            // 队列里没有钉住 session 的残留
}

// ── 7. IMAP IDLE：撤看门狗 + DONE 重新 arm ───────────────────────
namespace {
struct ImapTestServer : mail_system::MailServerBase {
    ImapTestServer(const mail_system::ServerConfig& c,
                   std::shared_ptr<pr::ThreadPoolBase> io,
                   std::shared_ptr<pr::ThreadPoolBase> w,
                   std::shared_ptr<mail_system::router::IShardRouter> r)
        : MailServerBase(c, io, w, std::move(r)) {}
    void start() override {}
    bool should_reject_connection(std::string&, const std::string&) const override { return false; }
};

struct ImapRig {
    std::shared_ptr<ManualExecutor> io = std::make_shared<ManualExecutor>();
    std::shared_ptr<QueuedPool> worker = std::make_shared<QueuedPool>();
    mail_system::ServerConfig cfg;
    std::shared_ptr<ImapTestServer> server;
    std::shared_ptr<mail_system::TraditionalImapsFsm<WatchdogWire>> fsm;
    WatchdogWire* wire = nullptr;   // 观察指针，所有权在 session 的 unique_ptr
    std::shared_ptr<mail_system::ImapsSession<WatchdogWire>> session;

    ImapRig() {
        cfg.perf_mode = true;
        cfg.apply_perf_mode();
        cfg.use_database = false;
        cfg.metrics_enabled = false;
        cfg.listeners.clear();
        cfg.system_domain = "test.local";
        server = std::make_shared<ImapTestServer>(cfg, worker, worker, make_router());
        fsm = std::make_shared<mail_system::TraditionalImapsFsm<WatchdogWire>>(worker, worker, make_router());
        auto conn_u = std::make_unique<WatchdogWire>(io);
        wire = conn_u.get();
        session = std::make_shared<mail_system::ImapsSession<WatchdogWire>>(
            server.get(), std::move(conn_u), fsm);
        auto* ctx = static_cast<mail_system::ImapContext*>(session->get_context());
        ctx->is_authenticated = true;
        session->set_current_state(static_cast<int>(mail_system::ImapState::AUTHENTICATED));
        // 手动武装看门狗（生产由 accept 路径的 start 完成）→ timers[0] 是看门狗
        session->rearm(session->config_read_timeout());
        io->drain();
    }

    mail_system::test::ManualTimer* watchdog_timer() {
        assert(!io->timers.empty());
        return io->timers.front().get();
    }
    std::weak_ptr<mail_system::ImapsSession<WatchdogWire>> finish() {
        std::weak_ptr<mail_system::ImapsSession<WatchdogWire>> weak = session;
        session->set_trace_clean_close();
        session->close();
        drain_quiet(io, worker);
        session.reset();
        io->drain();
        return weak;
    }
};
} // namespace

static void test_imap_idle_disarms_and_done_rearms() {
    ImapRig rig;
    auto* watchdog = rig.watchdog_timer();
    assert(watchdog->waits == 1 && watchdog->handler != nullptr);

    rig.fsm->process_event(rig.session, mail_system::ImapEvent::IDLE, "W1");
    drain_quiet(rig.io, rig.worker);
    assert(rig.wire->written().find("+ idling") != std::string::npos);
    assert(watchdog->cancels == 1);           // IDLE 撤看门狗
    assert(watchdog->handler == nullptr);     // 无任何武装的 wait：长空闲不误回收
    assert(!rig.session->close_requested());
    assert(rig.session->is_closed() == false);
    // 未选中邮箱 → 不注册推送 watcher（fixture 同款前置状态）
    assert(mail_system::IdlePushRegistry::instance().watcher_count("wd-user@test.local") == 0);

    rig.fsm->process_event(rig.session, mail_system::ImapEvent::DONE, "W1");
    drain_quiet(rig.io, rig.worker);
    assert(rig.wire->written().find("W1 OK IDLE terminated") != std::string::npos);
    assert(watchdog->waits == 2);             // DONE 重新 arm
    assert(watchdog->handler != nullptr);
    assert(watchdog->delay == rig.session->config_read_timeout());
    auto weak = rig.finish();
    assert(weak.expired());
}

int main() {
    mail_system::Logger::get_instance().set_handler({});
    test_start_arms_and_silent_peer_recycled();
    test_partial_line_no_rearm_slowloris_killed();
    test_slow_commands_idle_rearm_no_total_budget();
    test_rearm_replaces_pending_window();
    test_rearm_from_foreign_thread_indirects_through_executor();
    test_close_cancels_watchdog_breaks_self_cycle();
    test_imap_idle_disarms_and_done_rearms();
    return 0;
}
