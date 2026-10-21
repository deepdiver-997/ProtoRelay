#include "test_temp_dir.h"
// POP3 FSM 单元测试 — MockConnection 零 I/O 验证状态转换和响应
// 覆盖 11 命令 + 状态错 + 大小写 + 未知命令 + 锁 + dot-stuffing。
// 仿 smtps_fsm_test.cpp / imaps_fsm_test.cpp 的夹具模式。
#undef NDEBUG
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "mail_system/back/mailServer/fsm/pop3/traditional_pop3_fsm.tpp"
#include "mail_system/back/mailServer/session/pop3_session.tpp"
#include "framework/session_base.tpp"
#include "framework/server_config.h"
#include "framework/thread_pool/io_thread_pool.h"
#include "framework/thread_pool/worker_thread_pool.h"
#include "mail_system/back/router/static_shard_router.h"
#include "mail_system/back/common/logger.h"
#include "mail_system/back/storage/local_file_storage_provider.h"
#include "mock_connection.h"
#include "mock_db_pool.h"
#include "manual_executor.h"
#include "mail_system/back/common/bcrypt.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace mail_system;

// ========== 测试夹具 ==========

struct TestServer : MailServerBase {
    TestServer(const ServerConfig& c,
               std::shared_ptr<pr::ThreadPoolBase> io,
               std::shared_ptr<pr::ThreadPoolBase> w,
               std::shared_ptr<router::IShardRouter> r)
        : MailServerBase(c, io, w, std::move(r)) {}
    void start() override {}
    bool should_reject_connection(std::string&, const std::string&) const override { return false; }
};

struct FsmTestFixture {
    static inline const std::string MAIL_DIR = test_support::temp_path("pop3_fsm_test_mail");
    static inline const std::string ATT_DIR  = test_support::temp_path("pop3_fsm_test_att");

    std::shared_ptr<pr::IOThreadPool> io_pool;
    std::shared_ptr<pr::WorkerThreadPool> worker_pool;
    std::shared_ptr<router::StaticShardRouter> router;
    std::shared_ptr<test::MockDbPool> db_pool;
    std::shared_ptr<test::MockDbConnection> db_conn;
    std::shared_ptr<storage::LocalFileStorageProvider> storage;
    std::shared_ptr<TestServer> server;
    std::shared_ptr<TraditionalPop3Fsm<MockConnection>> fsm;
    ServerConfig cfg;

    FsmTestFixture() {
        Logger::get_instance().init("pop3_fsm_test.log", 0, 1, pr::LogLevel::off, false, false);
        io_pool     = std::make_shared<pr::IOThreadPool>(1);
        worker_pool = std::make_shared<pr::WorkerThreadPool>(2);
        io_pool->start();
        worker_pool->start();

        db_pool = std::make_shared<test::MockDbPool>();
        db_conn = db_pool->mock_conn();
        db_conn->set_deferred(false);   // 同步模式：async_query 回调在返回前触发（sq sync-bridge）

        storage = std::make_shared<storage::LocalFileStorageProvider>(MAIL_DIR, ATT_DIR);
        router = std::make_shared<router::StaticShardRouter>(
            std::vector<std::pair<std::string, int>>{},
            0,
            std::vector<std::shared_ptr<DBPool>>{db_pool},
            std::vector<std::shared_ptr<storage::IStorageProvider>>{storage});

        cfg.perf_mode    = true;
        cfg.apply_perf_mode();
        cfg.use_database = false;
        cfg.system_domain = "test.local";
        cfg.storage.local.mail_path       = MAIL_DIR;
        cfg.storage.local.attachment_path = ATT_DIR;

        std::filesystem::create_directories(MAIL_DIR);
        std::filesystem::create_directories(ATT_DIR);
        // 两封邮件的 body 文件；1002 首行以 '.' 开头，验 dot-stuffing
        {
            std::ofstream f1(std::string(MAIL_DIR) + "/1001", std::ios::binary);
            f1 << "hello from mail 1001\n";
        }
        {
            std::ofstream f2(std::string(MAIL_DIR) + "/1002", std::ios::binary);
            f2 << ".dot line\nnormal line\n";
        }

        server = std::shared_ptr<TestServer>(new TestServer(cfg, io_pool, worker_pool, router));
        fsm = std::make_shared<TraditionalPop3Fsm<MockConnection>>(
            io_pool, worker_pool, router);

        // 注入 auth 缓存（明文 test123，不走 DB）
        {
            AuthCacheEntry e;
            e.password_hash = "test123";
            e.status = 1;
            e.user_id = 1001;
            e.shard = 0;
            fsm->m_authCache->inject("alice@test.local", e);
        }
        {
            AuthCacheEntry e;
            e.password_hash = "test123";
            e.status = 1;
            e.user_id = 1002;
            e.shard = 0;
            fsm->m_authCache->inject("bob@test.local", e);
        }
    }

    ~FsmTestFixture() {
        worker_pool->stop();
        io_pool->stop();
        std::filesystem::remove_all(MAIL_DIR);
        std::filesystem::remove_all(ATT_DIR);
    }

    struct Handle {
        MockConnection* conn;
        std::shared_ptr<Pop3Session<MockConnection>> session;
        // deferred read 会让 greeting 之后的 do_async_read 挂起一个 pending handler
        // （捕获 shared_from_this），若不关闭会形成引用环 → LSan 泄漏。析构幂等关闭。
        // 收尾用 finish_session（close + 排空 + 等引用收敛）：POP3 的会话常由
        // worker 线程的 DB/心跳续作关闭，其后续 post 会晚于单次排空到达。
        ~Handle() { finish_session(session); }
    };

    Handle make_session() {
        auto conn_u = std::make_unique<MockConnection>();
        auto* conn_ptr = conn_u.get();
        // 关键：deferred read 让 async_read 无数据时挂起而非返回 EOF，
        // 否则 greeting 写完成后的 do_async_read 读到空缓冲 → EOF → 会话被提前关闭。
        conn_ptr->set_deferred_read(true);
        auto session = std::make_shared<Pop3Session<MockConnection>>(
            server.get(), std::move(conn_u), fsm);
        // 发 +OK banner → AUTHORIZATION
        Pop3Session<MockConnection>::start(session);
        conn_ptr->pump_executor();   // greeting 写经 exec_ctx_ 串行化(09-06),需排空才可见
        return {conn_ptr, session};
    }

    // 喂一条命令（模拟 read loop 的 extract_one_line → handle_read → process_read）
    void cmd(Handle& h, const std::string& line) {
        h.session->handle_read(line + "\r\n");
        h.session->process_read();
        h.conn->pump_executor();   // 响应写经 exec_ctx_ 串行化(09-06),需排空才可见
    }

    // 播种 DB：成功 PASS 依次消费 3 个查询结果（inbox_id → lock verify → mails）
    void seed_inbox(uint64_t inbox_id = 5001) {
        db_conn->push_sync_result(std::make_shared<test::MockDbResult>(
            std::vector<std::map<std::string, std::string>>{{{"id", std::to_string(inbox_id)}}}));
        db_conn->push_sync_result(std::make_shared<test::MockDbResult>(
            std::vector<std::map<std::string, std::string>>{{{"cnt", "1"}}}));
        db_conn->push_sync_result(std::make_shared<test::MockDbResult>(
            std::vector<std::map<std::string, std::string>>{
                {{"id", "1001"}, {"body_path", std::string(MAIL_DIR) + "/1001"}},
                {{"id", "1002"}, {"body_path", std::string(MAIL_DIR) + "/1002"}},
            }));
    }

    // 登录到 TRANSACTION
    Handle login() {
        auto h = make_session();
        cmd(h, "USER alice@test.local");
        seed_inbox();
        cmd(h, "PASS test123");
        wait_for(h, "+OK Mailbox locked and loaded");
        return h;
    }

    template <typename H>
    static bool wait_for(H& h, const std::string& needle, int timeout_ms = 2000) {
        for (int waited = 0; waited < timeout_ms; waited += 5) {
            h.conn->pump_executor();   // 异步回复落在 exec_ctx_ 队列,轮询需排空
            if (h.conn->written().find(needle) != std::string::npos) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        h.conn->pump_executor();
        return h.conn->written().find(needle) != std::string::npos;
    }

    template <typename H>
    static bool wait_closed(H& h, int timeout_ms = 3000) {
        for (int waited = 0; waited < timeout_ms; waited += 5) {
            h.conn->pump_executor();   // 关闭经 exec_ctx_ 串行化,排空后再判
            if (h.session->is_closed()) {
                h.conn->pump_executor(); // Also finish heartbeat cancellation.
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return h.session->is_closed();
    }
};

#define TEST(name) void test_##name(FsmTestFixture& fx)
#define HAS(str, sub) ((str).find(sub) != std::string::npos)

// ========== AUTHORIZATION 阶段 ==========

TEST(greeting) {
    auto h = fx.make_session();
    auto w = h.conn->written();
    assert(HAS(w, "+OK"));
    assert(HAS(w, "POP3 server ready"));
    std::cout << "  [PASS] greeting: " << w.substr(0, w.find('\r')) << std::endl;
}

TEST(capa) {
    auto h = fx.make_session();
    fx.cmd(h, "CAPA");
    auto w = h.conn->written();
    assert(HAS(w, "+OK Capability list follows"));
    assert(HAS(w, "USER"));
    assert(HAS(w, "UIDL"));
    assert(HAS(w, "."));
    std::cout << "  [PASS] capa" << std::endl;
}

TEST(user_ok) {
    auto h = fx.make_session();
    fx.cmd(h, "USER alice@test.local");
    assert(HAS(h.conn->written(), "+OK Send PASS"));
    std::cout << "  [PASS] user_ok" << std::endl;
}

TEST(user_no_arg) {
    auto h = fx.make_session();
    fx.cmd(h, "USER");
    assert(HAS(h.conn->written(), "-ERR"));
    std::cout << "  [PASS] user_no_arg" << std::endl;
}

TEST(pass_without_user) {
    auto h = fx.make_session();
    fx.cmd(h, "PASS x");
    assert(HAS(h.conn->written(), "-ERR USER first"));
    std::cout << "  [PASS] pass_without_user" << std::endl;
}

TEST(pass_wrong_password) {
    auto h = fx.make_session();
    fx.cmd(h, "USER alice@test.local");
    fx.cmd(h, "PASS wrongpass");
    assert(FsmTestFixture::wait_for(h, "-ERR Authentication failed"));
    // 仍在 AUTHORIZATION，可重试
    assert(!HAS(h.conn->written(), "Mailbox locked"));
    std::cout << "  [PASS] pass_wrong_password" << std::endl;
}

TEST(pass_three_failures_close) {
    auto h = fx.make_session();
    fx.cmd(h, "USER alice@test.local");
    fx.cmd(h, "PASS bad1");
    FsmTestFixture::wait_for(h, "-ERR Authentication failed");
    fx.cmd(h, "PASS bad2");
    FsmTestFixture::wait_for(h, "-ERR Authentication failed");
    fx.cmd(h, "PASS bad3");
    if (!FsmTestFixture::wait_for(h, "Too many auth failures", 10000)) {
        auto w = h.conn->written();
        std::cerr << "  DIAG written tail: [" << w.substr(w.size() > 400 ? w.size() - 400 : 0)
                  << "] closed=" << h.session->is_closed() << std::endl;
    }
    assert(FsmTestFixture::wait_for(h, "Too many auth failures"));
    assert(h.session->is_closed());
    std::cout << "  [PASS] pass_three_failures_close" << std::endl;
}

TEST(pass_user_not_found) {
    auto h = fx.make_session();
    fx.cmd(h, "USER nobody@test.local");
    // auth cache miss → DB 查，推一个空结果（0 行）
    fx.db_conn->push_sync_result(std::make_shared<test::MockDbResult>());
    fx.cmd(h, "PASS x");
    assert(FsmTestFixture::wait_for(h, "-ERR Authentication failed"));
    std::cout << "  [PASS] pass_user_not_found" << std::endl;
}

TEST(quit_in_authorization) {
    auto h = fx.make_session();
    fx.cmd(h, "QUIT");
    assert(HAS(h.conn->written(), "+OK Bye"));
    std::cout << "  [PASS] quit_in_authorization" << std::endl;
}

// ========== TRANSACTION 阶段 ==========

TEST(pass_success) {
    auto h = fx.login();
    auto w = h.conn->written();
    assert(HAS(w, "+OK Mailbox locked and loaded, 2 messages"));
    std::cout << "  [PASS] pass_success" << std::endl;
}

TEST(stat) {
    auto h = fx.login();
    h.conn->clear_written();
    fx.cmd(h, "STAT");
    auto w = h.conn->written();
    assert(HAS(w, "+OK 2 "));
    assert(HAS(w, "43"));   // 21 + 22 octets
    std::cout << "  [PASS] stat: " << w.substr(0, w.find('\r')) << std::endl;
}

TEST(list_all) {
    auto h = fx.login();
    h.conn->clear_written();
    fx.cmd(h, "LIST");
    auto w = h.conn->written();
    assert(HAS(w, "+OK 2 messages (43 octets)"));
    assert(HAS(w, "1 21"));
    assert(HAS(w, "2 22"));
    assert(HAS(w, "\r\n.\r\n"));   // 终止符
    std::cout << "  [PASS] list_all" << std::endl;
}

TEST(list_single) {
    auto h = fx.login();
    h.conn->clear_written();
    fx.cmd(h, "LIST 2");
    assert(HAS(h.conn->written(), "+OK 2 22"));
    std::cout << "  [PASS] list_single" << std::endl;
}

TEST(list_out_of_range) {
    auto h = fx.login();
    h.conn->clear_written();
    fx.cmd(h, "LIST 9");
    assert(HAS(h.conn->written(), "-ERR no such message"));
    std::cout << "  [PASS] list_out_of_range" << std::endl;
}

TEST(uidl_all) {
    auto h = fx.login();
    h.conn->clear_written();
    fx.cmd(h, "UIDL");
    auto w = h.conn->written();
    assert(HAS(w, "+OK Unique-ID listing follows"));
    assert(HAS(w, "1 1001"));
    assert(HAS(w, "2 1002"));
    assert(HAS(w, "\r\n.\r\n"));
    std::cout << "  [PASS] uidl_all" << std::endl;
}

TEST(uidl_single) {
    auto h = fx.login();
    h.conn->clear_written();
    fx.cmd(h, "UIDL 2");
    assert(HAS(h.conn->written(), "+OK 2 1002"));
    std::cout << "  [PASS] uidl_single" << std::endl;
}

TEST(retr_dot_stuffing) {
    auto h = fx.login();
    h.conn->clear_written();
    fx.cmd(h, "RETR 2");
    assert(FsmTestFixture::wait_for(h, "+OK 22 octets"));
    auto w = h.conn->written();
    assert(HAS(w, "..dot line"));   // dot-stuffing：行首 '.' 前缀 '.'
    assert(HAS(w, "normal line"));
    assert(HAS(w, "\r\n.\r\n"));    // 终止符
    std::cout << "  [PASS] retr_dot_stuffing" << std::endl;
}

TEST(retr_out_of_range) {
    auto h = fx.login();
    h.conn->clear_written();
    fx.cmd(h, "RETR 9");
    assert(HAS(h.conn->written(), "-ERR no such message"));
    std::cout << "  [PASS] retr_out_of_range" << std::endl;
}

TEST(dele_then_stat) {
    auto h = fx.login();
    h.conn->clear_written();
    fx.cmd(h, "DELE 1");
    assert(HAS(h.conn->written(), "+OK message 1 deleted"));
    h.conn->clear_written();
    fx.cmd(h, "STAT");
    assert(HAS(h.conn->written(), "+OK 1 "));   // 只剩 1 封
    std::cout << "  [PASS] dele_then_stat" << std::endl;
}

TEST(dele_rset_restore) {
    auto h = fx.login();
    h.conn->clear_written();
    fx.cmd(h, "DELE 1");
    fx.cmd(h, "RSET");
    assert(HAS(h.conn->written(), "+OK"));
    h.conn->clear_written();
    fx.cmd(h, "STAT");
    assert(HAS(h.conn->written(), "+OK 2 "));   // RSET 恢复
    std::cout << "  [PASS] dele_rset_restore" << std::endl;
}

TEST(dele_out_of_range) {
    auto h = fx.login();
    h.conn->clear_written();
    fx.cmd(h, "DELE 9");
    assert(HAS(h.conn->written(), "-ERR no such message"));
    std::cout << "  [PASS] dele_out_of_range" << std::endl;
}

TEST(noop) {
    auto h = fx.login();
    h.conn->clear_written();
    fx.cmd(h, "NOOP");
    assert(HAS(h.conn->written(), "+OK"));
    std::cout << "  [PASS] noop" << std::endl;
}

TEST(quit_in_transaction) {
    auto h = fx.login();
    h.conn->clear_written();
    fx.cmd(h, "DELE 1");
    fx.cmd(h, "QUIT");
    assert(FsmTestFixture::wait_for(h, "+OK Bye"));
    std::cout << "  [PASS] quit_in_transaction" << std::endl;
}

// ========== 边界 ==========

TEST(wrong_state_command) {
    auto h = fx.make_session();   // AUTHORIZATION
    fx.cmd(h, "STAT");
    assert(HAS(h.conn->written(), "-ERR Command not allowed in current state"));
    std::cout << "  [PASS] wrong_state_command" << std::endl;
}

TEST(command_case_insensitive) {
    auto h = fx.make_session();
    fx.cmd(h, "capa");
    assert(HAS(h.conn->written(), "+OK Capability list follows"));
    h.conn->clear_written();
    fx.cmd(h, "User alice@test.local");
    assert(HAS(h.conn->written(), "+OK Send PASS"));
    std::cout << "  [PASS] command_case_insensitive" << std::endl;
}

TEST(unknown_command) {
    auto h = fx.make_session();
    fx.cmd(h, "FOOBAR");
    assert(HAS(h.conn->written(), "-ERR Unknown command"));
    assert(!h.session->is_closed());   // 不关闭，可继续
    std::cout << "  [PASS] unknown_command" << std::endl;
}

TEST(extra_args_ignored) {
    auto h = fx.make_session();
    fx.cmd(h, "USER alice@test.local extra");
    assert(HAS(h.conn->written(), "+OK Send PASS"));
    std::cout << "  [PASS] extra_args_ignored" << std::endl;
}

// ========== 锁心跳（v2） ==========

TEST(heartbeat_timer_armed) {
    auto h = fx.login();
    auto* ctx = static_cast<Pop3Context*>(h.session->get_context());
    assert(ctx->heartbeat_timer != nullptr);
    assert(ctx->heartbeat_handler != nullptr);
    assert(!ctx->session_id.empty());
    std::cout << "  [PASS] heartbeat_timer_armed" << std::endl;
}

TEST(heartbeat_renew_keeps_lock) {
    auto h = fx.login();
    auto* ctx = static_cast<Pop3Context*>(h.session->get_context());
    // 续约 verify 得 cnt=1 → 锁还在（async CPS 版：mock 同步触发回调）
    fx.db_conn->push_sync_result(std::make_shared<test::MockDbResult>(
        std::vector<std::map<std::string, std::string>>{{{"cnt", "1"}}}));
    bool ok = false;
    TraditionalPop3Fsm<MockConnection>::renew_lock_heartbeat_async(
        fx.router, ctx->user_id, ctx->session_id, ctx->shard_index,
        [&ok](bool v) { ok = v; });
    assert(ok);
    assert(!h.session->is_closed());
    std::cout << "  [PASS] heartbeat_renew_keeps_lock" << std::endl;
}

TEST(heartbeat_renew_lock_lost) {
    auto h = fx.login();
    auto* ctx = static_cast<Pop3Context*>(h.session->get_context());
    // 行已不存在（被 sweeper 回收）→ verify 得 0 → 续约失败
    fx.db_conn->push_sync_result(std::make_shared<test::MockDbResult>());
    bool ok = true;
    TraditionalPop3Fsm<MockConnection>::renew_lock_heartbeat_async(
        fx.router, ctx->user_id, ctx->session_id, ctx->shard_index,
        [&ok](bool v) { ok = v; });
    assert(!ok);
    std::cout << "  [PASS] heartbeat_renew_lock_lost" << std::endl;
}

TEST(heartbeat_lock_lost_closes) {
    // 剧情：会话拿到锁后，锁被外部回收（如 sweeper 清掉死锁后他人接管）。
    // 心跳续约 verify 得 0 → 本会话失去排他 → 必须关闭，而不是带着失效的锁继续。
    fx.fsm->heartbeat_interval_ = std::chrono::milliseconds(50);
    auto h = fx.login();
    // 显式推空结果：保证首次续约 verify 得 0（不依赖队列残留状态）
    fx.db_conn->push_sync_result(std::make_shared<test::MockDbResult>());
    assert(FsmTestFixture::wait_closed(h));
    auto* ctx = static_cast<Pop3Context*>(h.session->get_context());
    assert(!ctx->heartbeat_timer);   // close() 已取消并复位定时器
    fx.fsm->heartbeat_interval_ = std::chrono::seconds(60);   // 复位，防影响后续测试
    std::cout << "  [PASS] heartbeat_lock_lost_closes" << std::endl;
}

TEST(heartbeat_stops_after_close) {
    auto h = fx.login();
    fx.cmd(h, "QUIT");
    assert(FsmTestFixture::wait_for(h, "+OK Bye"));
    assert(FsmTestFixture::wait_closed(h));
    auto* ctx = static_cast<Pop3Context*>(h.session->get_context());
    assert(!ctx->heartbeat_timer);   // QUIT 关闭会话时取消心跳
    std::cout << "  [PASS] heartbeat_stops_after_close" << std::endl;
}

TEST(sweep_expired_locks_ok) {
    // sweeper：对所有 shard 删除过期锁（mock execute 返回 true）
    bool ok = TraditionalPop3Fsm<MockConnection>::sweep_expired_locks(fx.router);
    assert(ok);
    std::cout << "  [PASS] sweep_expired_locks_ok" << std::endl;
}

// Deterministic application-boundary tests: no sleeps or live DB. Completions
// are explicitly delivered outside the connection owner to verify affinity.
class Pop3ManualWorkers final : public pr::ThreadPoolBase {
public:
    std::deque<std::function<void()>> tasks;
    void start() override {}
    void stop(bool = true) override {}
    size_t thread_count() const override { return 1; }
    bool is_running() const override { return true; }
    void drain() { while (!tasks.empty()) { auto task = std::move(tasks.front()); tasks.pop_front(); task(); } }
protected:
    void post_impl(std::function<void()> task) override { tasks.push_back(std::move(task)); }
};
class Pop3InjectedAuth final : public IAccountAuthenticator {
public:
    bool throws = false;
    void login_async(const std::string&, const std::string&, Complete cb, bool = false) override {
        if (throws) throw std::runtime_error("auth failed");
        AccountAuthResult result; result.ok = true; result.user_id = 41; cb(std::move(result));
    }
};
class Pop3InjectedMailboxes final : public repository::IPop3MailboxRepository {
public:
    InboxComplete inbox;
    MessagesComplete messages;
    DeleteComplete deletion;
    repository::Pop3DeletionRequest request;
    uint64_t user = 0, mailbox = 0;
    int deletes = 0;
    bool throw_inbox = false, throw_messages = false, throw_delete = false;
    void get_inbox_async(repository::Pop3DbContext context, uint64_t id, InboxComplete cb) override {
        assert(context && context->valid() && context->pool); user = id;
        if (throw_inbox) throw std::runtime_error("inbox failed");
        inbox = [context, cb = std::move(cb)](bool ok, uint64_t id) { cb(ok, id); };
    }
    void get_messages_async(repository::Pop3DbContext context, uint64_t id, uint64_t box, MessagesComplete cb) override {
        assert(context && context->valid() && context->pool); user = id; mailbox = box;
        if (throw_messages) throw std::runtime_error("messages failed");
        messages = [context, cb = std::move(cb)](bool ok, std::vector<MailboxMailInfo> value) { cb(ok, std::move(value)); };
    }
    void apply_deletions_async(repository::Pop3DbContext context, repository::Pop3DeletionRequest value, DeleteComplete cb) override {
        assert(context && context->valid() && context->pool); ++deletes; request = std::move(value);
        if (throw_delete) throw std::runtime_error("deletion failed");
        deletion = [context, cb = std::move(cb)](repository::Pop3DeletionResult value) { cb(value); };
    }
    void finish_inbox(bool ok, uint64_t id) { auto cb = std::move(inbox); assert(cb); cb(ok, id); }
    void finish_messages(bool ok, std::vector<MailboxMailInfo> rows = {}) { auto cb = std::move(messages); assert(cb); cb(ok, std::move(rows)); }
    void finish_delete(bool ok) { auto cb = std::move(deletion); assert(cb); cb({ok, true}); }
};
class Pop3InjectedLocks final : public repository::IPop3LockRepository {
public:
    Complete acquired, renewed;
    ReleaseComplete released;
    repository::Pop3LockKey key;
    int acquires = 0, renews = 0, releases = 0;
    bool throw_acquire = false, throw_renew = false, throw_release = false;
    void acquire_async(repository::Pop3DbContext context, repository::Pop3LockKey value, Complete cb) override {
        assert(context && context->valid() && context->pool); ++acquires; key = std::move(value);
        if (throw_acquire) throw std::runtime_error("acquire failed");
        acquired = [context, cb = std::move(cb)](repository::Pop3LockStatus status) { cb(status); };
    }
    void verify_async(repository::Pop3DbContext, repository::Pop3LockKey, Complete) override { assert(false); }
    void renew_async(repository::Pop3DbContext context, repository::Pop3LockKey value, Complete cb) override {
        assert(context && context->valid() && context->pool); ++renews; key = std::move(value);
        if (throw_renew) throw std::runtime_error("renew failed");
        renewed = [context, cb = std::move(cb)](repository::Pop3LockStatus status) { cb(status); };
    }
    void release_async(repository::Pop3DbContext context, repository::Pop3LockKey value, ReleaseComplete cb) override {
        assert(context && context->valid() && context->pool); ++releases; key = std::move(value);
        if (throw_release) throw std::runtime_error("release failed");
        released = [context, cb = std::move(cb)](bool ok) { cb(ok); };
    }
    bool sweep_expired(repository::Pop3DbContext) override { return true; }
    void finish_acquire(repository::Pop3LockStatus status) { auto cb = std::move(acquired); assert(cb); cb(status); }
    void finish_renew(repository::Pop3LockStatus status) { auto cb = std::move(renewed); assert(cb); cb(status); }
    void finish_release(bool ok) { auto cb = std::move(released); assert(cb); cb(ok); }
};
class ThrowingPop3Storage final : public pr::storage::IStorageProvider {
public:
    bool ensure_ready(pr::IoError&) override { return true; }
    bool append_binary(const std::string&, const char*, size_t, pr::IoError&) override { return false; }
    bool remove_object(const std::string&, pr::IoError&) override { return false; }
    bool read_all(const std::string&, std::string&, pr::IoError&) override { return false; }
    bool object_size(const std::string&, uint64_t&, pr::IoError&) override { throw std::runtime_error("size probe failed"); }
};
struct Pop3ProtocolFixture {
    using Fsm = TraditionalPop3Fsm<MockConnection>;
    std::shared_ptr<test::ManualExecutor> owner = std::make_shared<test::ManualExecutor>();
    std::shared_ptr<Pop3ManualWorkers> worker = std::make_shared<Pop3ManualWorkers>();
    std::shared_ptr<test::MockDbPool> pool = std::make_shared<test::MockDbPool>();
    std::shared_ptr<Pop3InjectedAuth> auth = std::make_shared<Pop3InjectedAuth>();
    std::shared_ptr<Pop3InjectedMailboxes> mailboxes = std::make_shared<Pop3InjectedMailboxes>();
    std::shared_ptr<Pop3InjectedLocks> locks = std::make_shared<Pop3InjectedLocks>();
    std::shared_ptr<router::StaticShardRouter> router;
    std::shared_ptr<Fsm> fsm;
    MockConnection* wire;
    std::shared_ptr<Pop3Session<MockConnection>> session;
    explicit Pop3ProtocolFixture(bool with_worker = true, bool with_router = true,
                                std::shared_ptr<pr::storage::IStorageProvider> provider = {},
                                std::shared_ptr<test::MockDbPool> custom_pool = {}) {
        if (custom_pool) pool = std::move(custom_pool);
        router = std::make_shared<router::StaticShardRouter>(std::vector<std::pair<std::string, int>>{}, 0,
            std::vector<std::shared_ptr<pr::DBPool>>{pool}, std::vector<std::shared_ptr<pr::storage::IStorageProvider>>{std::move(provider)});
        fsm = std::make_shared<Fsm>(worker, with_worker ? worker : nullptr, with_router ? router : nullptr);
        fsm->set_authenticator(auth);
        fsm->set_mailbox_repository(mailboxes); fsm->set_lock_repository(locks);
        auto connection = std::make_unique<MockConnection>(owner); wire = connection.get();
        wire->set_deferred_read(true);
        session = std::make_shared<Pop3Session<MockConnection>>(nullptr, std::move(connection), fsm);
        session->set_current_state(static_cast<int>(Pop3State::AUTHORIZATION));
        context()->username = "owner@test";
    }
    ~Pop3ProtocolFixture() {
        session->set_trace_clean_close(); session->close(); owner->drain();
        session.reset(); owner->drain();
    }
    Pop3Context* context() { return static_cast<Pop3Context*>(session->get_context()); }
    void command(std::string line) {
        owner->post([this, line = std::move(line)] { session->parse_pop3_command(line + "\r\n"); session->process_read(); });
        owner->drain();
    }
    void begin_login() { command("PASS secret"); worker->drain(); assert(mailboxes->inbox && session->is_paused()); }
    void acquire_login() {
        begin_login(); mailboxes->finish_inbox(true, 7);
        assert(mailboxes->user == 41 && locks->acquires == 1 && !locks->key.session_id.empty());
        locks->finish_acquire(repository::Pop3LockStatus::owned);
        assert(mailboxes->messages && mailboxes->mailbox == 7);
    }
    void complete_login() {
        acquire_login();
        MailboxMailInfo mail; mail.mail_id = 51; mail.body_path = "opaque/51"; mail.is_deleted = true;
        mailboxes->finish_messages(true, {mail}); worker->drain();
        assert(!context()->is_authenticated && wire->written().empty());
        owner->drain();
        assert(context()->is_authenticated && context()->messages.size() == 1 && context()->messages[0].mail_id == 51);
        assert(HAS(wire->written(), "+OK Mailbox locked and loaded") && context()->heartbeat_timer);
        wire->clear_written();
    }
    void transaction() {
        context()->is_authenticated = true; context()->user_id = 41; context()->mailbox_id = 7;
        context()->session_id = "session-A"; context()->messages.push_back({51, "opaque/51", 10});
        session->set_current_state(static_cast<int>(Pop3State::TRANSACTION));
    }
};
TEST(injected_quit_reports_write_failures_on_owner) {
    (void)fx;
    for (bool deletion_ok : {false, true}) for (bool release_ok : {false, true}) {
        Pop3ProtocolFixture fixture; fixture.transaction(); fixture.context()->deleted.insert(51);
        fixture.command("QUIT"); fixture.worker->drain();
        assert(fixture.mailboxes->deletes == 1 && fixture.locks->releases == 0);
        const auto& request = fixture.mailboxes->request;
        assert(request.user_id == 41 && request.mailbox_id == 7 && request.mail_ids == std::set<uint64_t>{51});
        fixture.mailboxes->finish_delete(deletion_ok);
        assert(fixture.locks->releases == 1 && fixture.locks->key.session_id == "session-A");
        fixture.locks->finish_release(release_ok);
        assert(fixture.wire->written().empty() && !fixture.session->is_closed() && fixture.session->is_paused());
        fixture.owner->drain();
        assert(fixture.session->is_closed() && !fixture.session->is_paused());
        assert(HAS(fixture.wire->written(), "+OK Bye") == (deletion_ok && release_ok));
        assert(HAS(fixture.wire->written(), "-ERR") == !(deletion_ok && release_ok));
    }
    std::cout << "  [PASS] injected QUIT reports deletion/release failures on owner" << std::endl;
}
TEST(quit_unavailable_resources_never_report_success) {
    (void)fx;
    for (int unavailable = 0; unavailable < 3; ++unavailable) {
        Pop3ProtocolFixture fixture(unavailable != 0, unavailable != 1);
        fixture.transaction(); fixture.context()->deleted.insert(51);
        if (unavailable == 2) fixture.pool->mock_conn()->disconnect();
        fixture.command("QUIT"); fixture.worker->drain(); fixture.owner->drain();
        assert(fixture.session->is_closed() && HAS(fixture.wire->written(), "-ERR"));
        assert(!HAS(fixture.wire->written(), "+OK Bye") && !fixture.mailboxes->deletes);
    }
    // An empty deletion set still completes lock release and succeeds normally.
    Pop3ProtocolFixture fixture;
    bool rejected = false;
    try { fixture.fsm->set_mailbox_repository({}); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected); rejected = false;
    try { fixture.fsm->set_lock_repository({}); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    fixture.transaction(); fixture.command("QUIT"); fixture.worker->drain();
    assert(fixture.mailboxes->request.mail_ids.empty()); fixture.mailboxes->finish_delete(true);
    fixture.locks->finish_release(true); fixture.owner->drain();
    assert(HAS(fixture.wire->written(), "+OK Bye"));
    std::cout << "  [PASS] QUIT fallback cannot claim an update succeeded" << std::endl;
}
TEST(pass_failure_and_disconnect_release_acquired_lock) {
    (void)fx;
    for (bool disconnected : {false, true}) {
        Pop3ProtocolFixture fixture; fixture.acquire_login();
        if (disconnected) { fixture.session->close(); fixture.owner->drain(); }
        fixture.mailboxes->finish_messages(false); fixture.worker->drain();
        assert(fixture.locks->releases == 1 && fixture.wire->written().empty());
        fixture.locks->finish_release(true);
        assert(fixture.wire->written().empty()); fixture.owner->drain();
        assert(!fixture.context()->is_authenticated);
        if (!disconnected) {
            assert(HAS(fixture.wire->written(), "-ERR Cannot load mailbox"));
            assert(!fixture.session->is_paused() && fixture.wire->has_pending_read());
        }
    }
    // Peer closes while acquisition itself is pending; its successful completion
    // must clean up without starting a mailbox read or authenticating the peer.
    {
        Pop3ProtocolFixture fixture; fixture.begin_login(); fixture.mailboxes->finish_inbox(true, 7);
        fixture.session->close(); fixture.owner->drain(); fixture.locks->finish_acquire(repository::Pop3LockStatus::owned);
        fixture.worker->drain(); assert(fixture.locks->releases == 1 && !fixture.mailboxes->messages);
        fixture.locks->finish_release(true); fixture.owner->drain(); assert(fixture.wire->written().empty());
    }
    {
        Pop3ProtocolFixture fixture; fixture.acquire_login();
        fixture.mailboxes->finish_messages(true); fixture.worker->drain();
        fixture.session->close(); fixture.owner->drain(); fixture.worker->drain();
        assert(fixture.locks->releases == 1 && !fixture.context()->is_authenticated);
        fixture.locks->finish_release(true); fixture.owner->drain(); assert(fixture.wire->written().empty());
    }
    for (bool read_ok : {false, true}) {
        Pop3ProtocolFixture fixture; fixture.begin_login(); fixture.mailboxes->finish_inbox(read_ok, 0);
        assert(fixture.wire->written().empty() && fixture.locks->acquires == 0);
        fixture.owner->drain();
        assert(HAS(fixture.wire->written(), read_ok ? "No INBOX for user" : "Server database unavailable"));
        assert(!fixture.session->is_paused());
    }
    // A verified conflict is not a DB outage; only an uncertain acquisition
    // needs scoped best-effort cleanup before the error reply.
    for (auto status : {repository::Pop3LockStatus::not_owned, repository::Pop3LockStatus::database_error}) {
        Pop3ProtocolFixture fixture; fixture.begin_login(); fixture.mailboxes->finish_inbox(true, 7);
        fixture.locks->finish_acquire(status); fixture.worker->drain();
        if (status == repository::Pop3LockStatus::database_error) fixture.locks->finish_release(false);
        fixture.owner->drain();
        assert(!fixture.context()->is_authenticated && !fixture.session->is_paused());
        assert(HAS(fixture.wire->written(), "[IN-USE]") == (status == repository::Pop3LockStatus::not_owned));
        assert(HAS(fixture.wire->written(), "-ERR"));
    }
    std::cout << "  [PASS] failed/abandoned PASS releases acquired or uncertain lock" << std::endl;
}
TEST(pass_storage_exception_releases_lock_and_resumes) {
    (void)fx;
    Pop3ProtocolFixture fixture(true, true, std::make_shared<ThrowingPop3Storage>());
    fixture.acquire_login(); MailboxMailInfo message; message.mail_id = 51; message.body_path = "opaque/51";
    fixture.mailboxes->finish_messages(true, {message}); fixture.worker->drain();
    assert(fixture.locks->releases == 1 && fixture.wire->written().empty() && fixture.session->is_paused());
    fixture.locks->finish_release(true); fixture.owner->drain();
    assert(!fixture.context()->is_authenticated && !fixture.session->is_paused());
    assert(HAS(fixture.wire->written(), "-ERR Cannot load mailbox") && fixture.wire->has_pending_read());
    std::cout << "  [PASS] delayed Worker storage exception releases lock and resumes PASS" << std::endl;
}
TEST(injected_heartbeat_and_quit_share_owner_policy) {
    (void)fx;
    for (bool quit_pending : {false, true}) {
        Pop3ProtocolFixture fixture; fixture.complete_login();
        auto timer = std::static_pointer_cast<test::ManualTimer>(fixture.context()->heartbeat_timer);
        timer->fire(); fixture.owner->drain(); fixture.worker->drain();
        assert(fixture.locks->renews == 1 && timer->waits == 1);
        if (quit_pending) { fixture.command("QUIT"); assert(timer->cancels == 1 && !fixture.context()->heartbeat_timer); }
        fixture.locks->finish_renew(repository::Pop3LockStatus::database_error);
        assert(!fixture.session->is_closed()); fixture.owner->drain();
        if (!quit_pending) { assert(fixture.session->is_closed() && timer->cancels == 1); }
        else {
            assert(!fixture.session->is_closed() && timer->waits == 1);
            fixture.worker->drain(); fixture.mailboxes->finish_delete(true); fixture.locks->finish_release(true); fixture.owner->drain();
            assert(HAS(fixture.wire->written(), "+OK Bye") && fixture.session->is_closed());
        }
    }
    std::cout << "  [PASS] heartbeat failure closes on owner; QUIT suppresses late renewal" << std::endl;
}

// Fixed capacity of one: a second checkout is rejected deterministically rather
// than blocking the test process. The real authenticator holds this lease while
// calling its completion, matching production's single-slot pool deadlock case.
class Pop3SingleSlotPool final : public test::MockDbPool {
public:
    bool checked_out = false, throw_checkout = false;
    int checkouts = 0, returns = 0, collisions = 0;
    std::shared_ptr<pr::IDBConnection> get_connection() override {
        if (throw_checkout) throw std::runtime_error("checkout failed");
        if (checked_out) { ++collisions; return {}; }
        checked_out = true; ++checkouts; return mock_conn();
    }
    void release_connection(std::shared_ptr<pr::IDBConnection>) override { assert(checked_out); checked_out = false; ++returns; }
};
TEST(pass_real_authenticator_releases_single_slot_before_mailbox) {
    (void)fx;
    auto pool = std::make_shared<Pop3SingleSlotPool>(); pool->mock_conn()->set_deferred(false);
    Pop3ProtocolFixture fixture(true, true, {}, pool);
    fixture.fsm->set_authenticator(std::make_shared<AccountAuthenticationService>(
        fixture.router, std::make_shared<AuthCache>(), fixture.worker));
    pool->mock_conn()->push_sync_result(std::make_shared<test::MockDbResult>(
        std::vector<std::map<std::string, std::string>>{{{"id", "41"}, {"status", "1"}, {"password", bcrypt_hash("secret", 4)}}}));
    fixture.begin_login();
    assert(pool->checkouts == 2 && pool->returns == 1 && !pool->collisions && pool->checked_out);
    fixture.mailboxes->finish_inbox(true, 7); fixture.locks->finish_acquire(repository::Pop3LockStatus::owned);
    fixture.mailboxes->finish_messages(true); fixture.worker->drain(); fixture.owner->drain();
    assert(fixture.context()->is_authenticated && HAS(fixture.wire->written(), "+OK Mailbox locked"));
    assert(pool->returns == 2 && !pool->checked_out && !pool->collisions);
    std::cout << "  [PASS] real authenticator releases single-slot lease before POP3 checkout" << std::endl;
}
TEST(pop3_async_continuation_exceptions_complete_once) {
    (void)fx;
    for (int stage = 0; stage < 5; ++stage) {
        Pop3ProtocolFixture fixture;
        if (stage == 0) fixture.auth->throws = true;
        if (stage == 1) fixture.mailboxes->throw_inbox = true;
        if (stage == 2) fixture.locks->throw_acquire = true;
        if (stage == 3) fixture.mailboxes->throw_messages = true;
        if (stage >= 4) fixture.locks->throw_release = true;
        fixture.command("PASS secret"); fixture.worker->drain();
        if (stage >= 2) fixture.mailboxes->finish_inbox(true, 7);
        if (stage >= 3) fixture.locks->finish_acquire(repository::Pop3LockStatus::owned);
        if (stage >= 4) fixture.mailboxes->finish_messages(false);
        fixture.worker->drain();
        if (stage == 2 || stage == 3) fixture.locks->finish_release(true);
        assert(fixture.wire->written().empty()); fixture.owner->drain();
        const auto reply = fixture.wire->written();
        assert(HAS(reply, "-ERR") && reply.find("-ERR", reply.find("-ERR") + 1) == std::string::npos);
        assert(!fixture.context()->is_authenticated && !fixture.session->is_paused());
        assert(!fixture.mailboxes->deletes);
    }
    for (bool release_throws : {false, true}) {
        Pop3ProtocolFixture fixture; fixture.transaction();
        fixture.mailboxes->throw_delete = !release_throws; fixture.locks->throw_release = release_throws;
        fixture.command("QUIT"); fixture.worker->drain();
        if (release_throws) fixture.mailboxes->finish_delete(true);
        else fixture.locks->finish_release(true);
        assert(fixture.wire->written().empty()); fixture.owner->drain();
        assert(fixture.session->is_closed() && HAS(fixture.wire->written(), "-ERR") && fixture.locks->releases == 1);
    }
    // Pool checkout exceptions are inside queued tasks, after post() has returned.
    for (bool quit : {false, true}) {
        auto pool = std::make_shared<Pop3SingleSlotPool>(); pool->throw_checkout = true;
        Pop3ProtocolFixture fixture(true, true, {}, pool);
        if (quit) fixture.transaction();
        fixture.command(quit ? "QUIT" : "PASS secret"); fixture.worker->drain(); fixture.owner->drain();
        assert(HAS(fixture.wire->written(), "-ERR") && !fixture.session->is_paused());
        assert(fixture.session->is_closed() == quit);
    }
    // Throwing renewal is also completed on the owner and never rearms a timer.
    {
        Pop3ProtocolFixture fixture; fixture.complete_login(); fixture.locks->throw_renew = true;
        auto timer = std::static_pointer_cast<test::ManualTimer>(fixture.context()->heartbeat_timer);
        timer->fire(); fixture.owner->drain(); fixture.worker->drain();
        assert(!fixture.session->is_closed()); fixture.owner->drain();
        assert(fixture.session->is_closed() && timer->cancels == 1 && timer->waits == 1);
    }
    std::cout << "  [PASS] POP3 delayed orchestration exceptions clean up and complete once" << std::endl;
}
TEST(retr_completion_resumes_only_on_owner) {
    (void)fx;
    Pop3ProtocolFixture fixture(true, true, std::make_shared<ThrowingPop3Storage>());
    fixture.transaction(); fixture.command("RETR 1"); fixture.worker->drain();
    assert(fixture.wire->written().empty() && fixture.session->is_paused()); fixture.owner->drain();
    assert(HAS(fixture.wire->written(), "-ERR Failed to read message body") && !fixture.session->is_paused());
    assert(fixture.wire->has_pending_read());
    std::cout << "  [PASS] RETR completion resumes on owner" << std::endl;
}

// ========== main ==========

int main() {
    FsmTestFixture fx;
    int passed = 0, failed = 0;
    auto run = [&](const char* name, void (*fn)(FsmTestFixture&)) {
        try {
            fn(fx);
            ++passed;
        } catch (const std::exception& e) {
            ++failed;
            std::cerr << "  [FAIL] " << name << ": " << e.what() << std::endl;
        } catch (...) {
            ++failed;
            std::cerr << "  [FAIL] " << name << ": unknown exception" << std::endl;
        }
    };

    run("greeting",                  test_greeting);
    run("capa",                      test_capa);
    run("user_ok",                   test_user_ok);
    run("user_no_arg",               test_user_no_arg);
    run("pass_without_user",         test_pass_without_user);
    run("pass_wrong_password",       test_pass_wrong_password);
    run("pass_three_failures_close", test_pass_three_failures_close);
    run("pass_user_not_found",       test_pass_user_not_found);
    run("quit_in_authorization",     test_quit_in_authorization);
    run("pass_success",              test_pass_success);
    run("stat",                      test_stat);
    run("list_all",                  test_list_all);
    run("list_single",               test_list_single);
    run("list_out_of_range",         test_list_out_of_range);
    run("uidl_all",                  test_uidl_all);
    run("uidl_single",               test_uidl_single);
    run("retr_dot_stuffing",         test_retr_dot_stuffing);
    run("retr_out_of_range",         test_retr_out_of_range);
    run("dele_then_stat",            test_dele_then_stat);
    run("dele_rset_restore",         test_dele_rset_restore);
    run("dele_out_of_range",         test_dele_out_of_range);
    run("noop",                      test_noop);
    run("quit_in_transaction",       test_quit_in_transaction);
    run("wrong_state_command",       test_wrong_state_command);
    run("command_case_insensitive",  test_command_case_insensitive);
    run("unknown_command",           test_unknown_command);
    run("extra_args_ignored",        test_extra_args_ignored);
    run("heartbeat_timer_armed",     test_heartbeat_timer_armed);
    run("heartbeat_renew_keeps_lock",test_heartbeat_renew_keeps_lock);
    run("heartbeat_renew_lock_lost", test_heartbeat_renew_lock_lost);
    run("heartbeat_lock_lost_closes",test_heartbeat_lock_lost_closes);
    run("heartbeat_stops_after_close",test_heartbeat_stops_after_close);
    run("sweep_expired_locks_ok",    test_sweep_expired_locks_ok);
    run("injected_quit_reports_write_failures_on_owner", test_injected_quit_reports_write_failures_on_owner);
    run("quit_unavailable_resources_never_report_success", test_quit_unavailable_resources_never_report_success);
    run("pass_failure_and_disconnect_release_acquired_lock", test_pass_failure_and_disconnect_release_acquired_lock);
    run("pass_storage_exception_releases_lock_and_resumes", test_pass_storage_exception_releases_lock_and_resumes);
    run("injected_heartbeat_and_quit_share_owner_policy", test_injected_heartbeat_and_quit_share_owner_policy);
    run("pass_real_authenticator_releases_single_slot_before_mailbox", test_pass_real_authenticator_releases_single_slot_before_mailbox);
    run("pop3_async_continuation_exceptions_complete_once", test_pop3_async_continuation_exceptions_complete_once);
    run("retr_completion_resumes_only_on_owner", test_retr_completion_resumes_only_on_owner);

    std::cout << "pop3_fsm_test: " << passed << " passed, " << failed << " failed" << std::endl;
    return failed == 0 ? 0 : 1;
}
