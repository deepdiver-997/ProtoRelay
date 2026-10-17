// SMTP 入站异步并发测试 — Mock DNS/DB 延迟回调 + 真实 IO 线程
//
// 目标：为入站校验/FSM 的异步路径（MAIL FROM SPF / RCPT user_exists / DATA_END verify_all）
// 建立真实多线程时序测试：
//   - session 的常规命令处理跑在 IOThreadPool 的 io_context 线程上；
//   - DNS/DB 异步回调由测试线程手动触发（模拟 c-ares / DB worker 线程）；
//   - 验证跨线程的 paused/resume、shared_ptr 保活（回调晚于 session 逻辑结束不悬垂）。
// 配合 TSan（-fsanitize=thread）可检测 session 状态竞争。
#undef NDEBUG
#include <cassert>  // 在 #undef NDEBUG 后重新包含，确保 assert 生效
#include "mail_system/back/mailServer/smtps_server.h"
#include "mail_system/back/mailServer/fsm/smtps/traditional_smtps_fsm.tpp"
#include "mail_system/back/mailServer/session/smtps_session.tpp"
#include "framework/session_base.tpp"
#include "framework/server_config.h"
#include "framework/thread_pool/io_thread_pool.h"
#include "framework/thread_pool/boost_thread_pool.h"
#include "mail_system/back/persist_storage/persistent_queue.h"
#include "mail_system/back/router/static_shard_router.h"
#include "mail_system/back/common/logger.h"
#include "mail_system/back/common/mail_crypto.h"
#include "mail_system/back/inbound/inbound_verifier.h"
#include "mock_connection.h"
#include "mock_dns_resolver.h"
#include "mock_db_pool.h"

#include <boost/asio/post.hpp>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <spdlog/common.h>

using namespace mail_system;
namespace pq = persist_storage;

// ========== 测试夹具 ==========

// 与 smtps_fsm_test 一致：必须派生自 SmtpsServer（SmtpsSession 构造用 static_cast 读 m_persistentQueue）。
struct TestServer : SmtpsServer {
    TestServer(const ServerConfig& c,
               std::shared_ptr<ThreadPoolBase> io,
               std::shared_ptr<ThreadPoolBase> w,
               std::shared_ptr<router::IShardRouter> r)
        : SmtpsServer(c, io, w, nullptr) {
        m_shardRouter = std::move(r);
    }
    void start() override {}
};

// 会话句柄：captured 为外部写缓冲镜像（session 析构后仍可断言）
struct SessionHandle {
    MockConnection* conn = nullptr;
    std::shared_ptr<SmtpsSession<MockConnection>> session;
    std::string captured;

    // 收尾（见 mock_connection.h 会话收尾契约）：close + 排空 + 等引用收敛。
    // 本套件必须收尾，原因是 ASan 抓到过的 heap-use-after-free：在途读完成的
    // 回调持着 session 存活，句柄不复位就一路活到 main 结束 —— 那时 fixture 的
    // server 已析构，回调里的 close() 摸 m_server->push_metric_observe 即为 UAF。
    // （无 ASan 时这次野写会砸坏 libgcov 的写盘缓冲，表现为 coverage 构建退出
    //   阶段 "Merge mismatch / Error writing" + SIGSEGV，见 2026-09-16 复盘。）
    // boundary_callback_after_session_release 用例已自行 reset() 并等待
    // weak 过期，此处对空 session 是 no-op。
    ~SessionHandle() { finish_session(session); }
};

struct ConcurrencyTestFixture {
    std::shared_ptr<IOThreadPool> io_pool;
    std::shared_ptr<BoostThreadPool> worker_pool;
    std::shared_ptr<test::MockDnsResolver> dns;
    std::shared_ptr<test::MockDbPool> db_pool;
    std::shared_ptr<router::StaticShardRouter> router;
    std::shared_ptr<pq::PersistentQueue> persist_q;
    std::shared_ptr<TestServer> server;
    std::shared_ptr<TraditionalSmtpsFsm<MockConnection>> fsm;
    ServerConfig cfg;

    ConcurrencyTestFixture() {
        Logger::get_instance().init("smtps_fsm_concurrency_test.log", 0, 1, spdlog::level::off, false, false);
        // 连接级异步 I/O 由 MockConnection 内部 MockIoContext 线程驱动（还原 asio 投递语义）。
        // io_pool 保留仅为满足 SmtpsServer/FSM 构造签名，不参与连接读写。
        io_pool     = std::make_shared<IOThreadPool>(1);
        worker_pool = std::make_shared<BoostThreadPool>(2);
        io_pool->start();
        worker_pool->start();

        dns     = std::make_shared<test::MockDnsResolver>();
        dns->set_mode(test::MockDnsResolver::Mode::Manual);   // 并发测试手动触发 DNS 回调
        db_pool = std::make_shared<test::MockDbPool>();

        router = std::make_shared<router::StaticShardRouter>(
            std::vector<std::pair<std::string, int>>{},
            0,
            std::vector<std::shared_ptr<DBPool>>{db_pool},  // shard 0 → mock DB（user_exists 异步路径）
            std::vector<std::shared_ptr<storage::IStorageProvider>>{});

        persist_q = std::make_shared<pq::PersistentQueue>(router, worker_pool);
        pq::PersistentQueuePressureConfig pc;
        pc.max_inflight_mails           = 1000000;
        pc.min_available_memory_mb      = 0;
        pc.min_db_available_connections = 0;
        persist_q->set_pressure_config(pc);
        persist_q->set_local_domain("test.local");

        cfg.perf_mode          = false;   // 允许 SPF/DKIM/DMARC 校验
        cfg.use_database       = false;
        cfg.inbound_ack_mode   = InboundAckMode::AFTER_ENQUEUE;
        cfg.system_domain      = "test.local";
        cfg.inbound_spf_mode   = "off";   // 测试按需开启（hard/pass）
        cfg.inbound_dkim_mode  = "off";
        cfg.inbound_dmarc_mode = "off";
        cfg.mail_storage_path  = "/tmp/smtps_fsm_conc_test_mail";
        cfg.attachment_storage_path = "/tmp/smtps_fsm_conc_test_att";
        cfg.storage.local.mail_path       = "/tmp/smtps_fsm_conc_test_mail";
        cfg.storage.local.attachment_path = "/tmp/smtps_fsm_conc_test_att";

        system("mkdir -p /tmp/smtps_fsm_conc_test_mail /tmp/smtps_fsm_conc_test_att");

        server = std::shared_ptr<TestServer>(new TestServer(cfg, io_pool, worker_pool, router));
        server->m_persistentQueue = persist_q;
        server->m_dnsResolver = dns;   // 注入 mock DNS → MAIL FROM SPF / DATA_END verify 路径

        fsm = std::make_shared<TraditionalSmtpsFsm<MockConnection>>(
            io_pool, worker_pool, persist_q, router);

        // 预注入收件人存在性缓存（RCPT 快速路径）
        auto inject_rcpt = [this](const std::string& email, int status) {
            AuthCacheEntry entry;
            entry.password_hash = "";
            entry.status = status;
            fsm->m_recipientCache->inject(email, entry);
        };
        inject_rcpt("rcpt@test.local", 1);
        inject_rcpt("rcpt1@test.local", 1);
        inject_rcpt("r1@test.local", 1);
    }

    ~ConcurrencyTestFixture() {
        // 确保 DB mock 回到同步模式（若还有 pending 持久化链，避免 worker 线程卡在未触发的回调上）
        db_pool->mock_conn()->set_deferred(false);
        persist_q->shutdown();
        worker_pool->stop();
        io_pool->stop();
        dns->join_all();
        system("rm -rf /tmp/smtps_fsm_conc_test_mail /tmp/smtps_fsm_conc_test_att");
    }

    // 重新应用配置（FSM 通过 atomic_load 读取 server->m_config）
    void apply_config(const ServerConfig& c) {
        cfg = c;
        std::atomic_store(&server->m_config, std::make_shared<ServerConfig>(c));
    }
    ServerConfig with_spf(const std::string& mode) {
        ServerConfig c = cfg;
        c.inbound_spf_mode = mode;
        return c;
    }

    std::shared_ptr<SessionHandle> make_session(const std::string& preload = "",
                                                InboundAuthPolicy auth_policy = InboundAuthPolicy::OFF) {
        auto h = std::make_shared<SessionHandle>();
        auto conn_u = std::make_unique<MockConnection>();
        h->conn = conn_u.get();
        h->conn->capture_to(&h->captured);   // 写缓冲镜像到外部 string（session 析构后仍可断言）
        if (!preload.empty())
            h->conn->set_read_data(preload);

        h->session = std::make_shared<SmtpsSession<MockConnection>>(
            server.get(), std::move(conn_u), fsm);

        ListenerConfig lc;
        lc.type        = ListenerType::TCP;
        lc.port        = 25;
        lc.auth_policy = auth_policy;
        h->session->set_listener_config(lc);
        h->session->set_current_state(static_cast<int>(SmtpsState::INIT));
        h->session->set_next_event(static_cast<int>(SmtpsEvent::CONNECT));
        return h;
    }

    // 启动连接的 MockIoContext 工作线程，并把 session 初始 process_read
    // 作为任务投递到该 context——由独立线程执行命令链（还原 asio 语义）。
    // 等待命令链完全暂停（context 空闲）后才返回，保证主线程触发异步回调时
    // context 线程已退出 session（与生产时序一致：io 线程已暂停，回调独占）。
    void run_on_io(const std::shared_ptr<SessionHandle>& h) {
        h->conn->start();                    // 线程模式：异步完成由独立线程投递
        h->conn->start_executor();           // exec_ctx_ 发起队列也要有人跑
        h->conn->context().post([h]() { h->session->process_read(); });
        assert(h->conn->wait_idle(3000));    // 命令链暂停后 context 空闲
    }
};

// ========== 测试宏 ==========
#define TEST(name) \
    void test_##name(ConcurrencyTestFixture& fx)
#define HAS(str, sub) ((str).find(sub) != std::string::npos)

// 带超时的轮询等待（异步回调/析构发生在其它线程）
template <typename Pred>
static bool wait_until(Pred pred, int timeout_ms = 3000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

// ========== 1. MAIL FROM SPF 异步路径（手动触发 DNS 回调） ==========

TEST(spf_hard_fail_async) {
    inbound::InboundVerifier::clear_dns_cache();
    fx.apply_config(fx.with_spf("hard"));
    fx.dns->set_txt("attacker.com", {"v=spf1 -all"});   // SPF hard-fail

    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<spam@attacker.com>\r\n");
    fx.run_on_io(h);

    // 等待 context 线程处理命令链至 SPF 暂停（DNS 回调 pending）
    assert(wait_until([&] { return fx.dns->has_pending_txt("attacker.com"); }));
    // 主线程手动触发 DNS 回调（模拟 c-ares 线程）→ 应在本线程恢复 session
    fx.dns->fire_txt("attacker.com");

    // 550 响应写经 exec_ctx_ 串行化,轮询等它上 wire
    assert(wait_until([&] { return HAS(h->captured, "550 5.7.1 SPF verification failed"); }));
    std::cout << "  [PASS] spf_hard_fail_async (cross-thread DNS callback)" << std::endl;
}

TEST(spf_pass_async) {
    inbound::InboundVerifier::clear_dns_cache();
    fx.apply_config(fx.with_spf("pass"));
    fx.dns->set_txt("sender.com", {"v=spf1 ip4:127.0.0.1"});   // 客户端 IP=127.0.0.1 → pass

    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<s@sender.com>\r\n"
        "RCPT TO:<rcpt@test.local>\r\n");
    fx.run_on_io(h);

    assert(wait_until([&] { return fx.dns->has_pending_txt("sender.com"); }));
    fx.dns->fire_txt("sender.com");

    // SPF pass → 250 Ok → drain 缓冲的 RCPT TO（缓存命中）→ 250
    assert(wait_until([&] { return HAS(h->captured, "250 Ok"); }));
    std::cout << "  [PASS] spf_pass_async (SPF 250 + 流水线续传)" << std::endl;
}

// ========== 2. RCPT user_exists DB 异步路径（延迟触发 async_query 回调） ==========

TEST(user_exists_db_async_exists) {
    fx.apply_config(fx.with_spf("off"));   // MAIL FROM 不走 SPF，保持流程直接到 RCPT
    fx.db_pool->mock_conn()->set_deferred(true);
    fx.db_pool->mock_conn()->clear_pending();   // 排除持久化 worker 等其它查询来源

    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<s@test.local>\r\n"
        "RCPT TO:<dbuser@test.local>\r\n");   // 缓存未命中 → DB 异步
    fx.run_on_io(h);

    auto* db = fx.db_pool->mock_conn().get();
    assert(wait_until([&] {
        return db->has_pending_query() &&
               !db->last_query_params().empty() && db->last_query_params()[0] == "dbuser@test.local";
    }));
    assert(!db->last_query_params().empty() && db->last_query_params()[0] == "dbuser@test.local");

    // 主线程触发 DB 回调：查询命中 status=1 → 存在 → 250
    db->fire_query(std::make_shared<test::MockDbResult>(
        std::vector<std::map<std::string, std::string>>{{{"status", "1"}}}));

    assert(wait_until([&] { return HAS(h->captured, "250 Ok"); }));
    std::cout << "  [PASS] user_exists_db_async_exists (DB 回调触发 → 250)" << std::endl;
}

TEST(user_exists_db_async_not_found) {
    fx.apply_config(fx.with_spf("off"));
    fx.db_pool->mock_conn()->set_deferred(true);
    fx.db_pool->mock_conn()->clear_pending();   // 排除持久化 worker 等其它查询来源

    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<s@test.local>\r\n"
        "RCPT TO:<ghost@test.local>\r\n");   // 缓存未命中 → DB 异步
    fx.run_on_io(h);

    auto* db = fx.db_pool->mock_conn().get();
    assert(wait_until([&] {
        return db->has_pending_query() &&
               !db->last_query_params().empty() && db->last_query_params()[0] == "ghost@test.local";
    }));

    // 空结果 → 不存在 → 550 User unknown（负缓存）
    db->fire_query(std::make_shared<test::MockDbResult>());

    assert(wait_until([&] { return HAS(h->captured, "550 5.1.1 User unknown"); }));
    std::cout << "  [PASS] user_exists_db_async_not_found (DB 回调触发 → 550)" << std::endl;
}

// ========== 2b. AUTH auth_user_async DB 异步路径 ==========
//
// 2026-10-17 起 DB 路径对 MD5/明文历史行多一跳：验密通过后先发 CAS 密码 UPDATE
// （rehash-on-login，见 common/password_rehash.h），再发 update_last_login，最后 235。
// 本测试固定这个顺序与 CAS 参数：少了旧值守卫 = 并发改密被 bcrypt(旧密码) 覆盖；
// 顺序反了 = 235 之前密码没升级。bcrypt 行"绝不多这一跳"的对照用例见下一个测试。

TEST(auth_user_db_async) {
    fx.apply_config(fx.with_spf("off"));
    fx.db_pool->mock_conn()->set_deferred(true);
    fx.db_pool->mock_conn()->clear_pending();   // 排除持久化 worker 等其它查询来源

    // dbuser@test.local 不在认证缓存 → 走 DB：
    //   async_query(取 password) → [明文行] async_execute(CAS 密码 UPDATE) → async_execute(update_last_login)
    std::string user_plain = "dbuser@test.local";
    std::string user_b64 = mail_system::outbound::base64_encode(
        reinterpret_cast<const unsigned char*>(user_plain.data()), user_plain.size());
    std::string pass_plain = "test123";
    std::string pass_b64 = mail_system::outbound::base64_encode(
        reinterpret_cast<const unsigned char*>(pass_plain.data()), pass_plain.size());

    auto h = fx.make_session(
        "EHLO test\r\n"
        "AUTH LOGIN\r\n"
        + user_b64 + "\r\n"
        + pass_b64 + "\r\n",
        InboundAuthPolicy::ON);
    fx.run_on_io(h);

    auto* db = fx.db_pool->mock_conn().get();
    // 第一步：auth_user_async 的 DB 查询 pending（密码哈希）
    assert(wait_until([&] {
        return db->has_pending_query() &&
               !db->last_query_params().empty() && db->last_query_params()[0] == "dbuser@test.local";
    }));
    // 触发查询：status=1 + 明文密码 test123 → verify_password_hash 通过
    db->fire_query(std::make_shared<test::MockDbResult>(
        std::vector<std::map<std::string, std::string>>{{{"status", "1"}, {"password", "test123"}}}));

    // 第二步：明文行 → rehash-on-login 先发 CAS 密码 UPDATE
    // （bcrypt_hash 在回调线程同步算 ~几百 ms，wait_until 3s 足够）
    assert(wait_until([&] { return db->has_pending_execute(); }));
    {
        const std::string sql = db->last_query_sql();
        const auto p = db->last_query_params();
        assert(HAS(sql, "SET password = ?"));
        assert(HAS(sql, "AND password = ?"));          // CAS 守卫：限定旧值
        assert(p.size() == 3);
        assert(is_bcrypt_hash(p[0]));                   // 新值是 bcrypt
        assert(bcrypt_verify("test123", p[0]));         // 且能验回原密码
        assert(p[1] == "dbuser@test.local");
        assert(p[2] == "test123");                      // 旧值 = 刚从 DB 读到的明文
    }
    db->fire_execute(true);

    // 第三步：升级落地后才发 update_last_login
    assert(wait_until([&] {
        return db->has_pending_execute() && HAS(db->last_query_sql(), "last_login_time");
    }));
    db->fire_execute(true);

    assert(wait_until([&] { return HAS(h->captured, "235 Authentication successful"); }));

    // 缓存条目已刷成 bcrypt：明文不再躺在 AuthCache 里一个 TTL
    AuthCacheEntry ce;
    assert(fx.fsm->m_authCache->lookup("dbuser@test.local", ce));
    assert(is_bcrypt_hash(ce.password_hash));
    assert(bcrypt_verify("test123", ce.password_hash));
    std::cout << "  [PASS] auth_user_db_async (明文行: SELECT → CAS 密码 UPDATE → last_login → 235, 缓存已升级)" << std::endl;
}

// 对照：bcrypt 行验密通过后唯一一次 execute 必须直接是 update_last_login，
// 不许出现密码 UPDATE，缓存条目原样保留（否则每次登录都白算一次 bcrypt 并写一次 DB）。
TEST(auth_user_db_async_bcrypt_no_rehash) {
    fx.apply_config(fx.with_spf("off"));
    fx.db_pool->mock_conn()->set_deferred(true);
    fx.db_pool->mock_conn()->clear_pending();

    std::string user_plain = "bcuser@test.local";   // 与上一用例不同账号：避免缓存命中走快路径
    std::string user_b64 = mail_system::outbound::base64_encode(
        reinterpret_cast<const unsigned char*>(user_plain.data()), user_plain.size());
    std::string pass_plain = "test123";
    std::string pass_b64 = mail_system::outbound::base64_encode(
        reinterpret_cast<const unsigned char*>(pass_plain.data()), pass_plain.size());
    const std::string stored_bcrypt = bcrypt_hash("test123", 4);   // 低 cost：验密快

    auto h = fx.make_session(
        "EHLO test\r\n"
        "AUTH LOGIN\r\n"
        + user_b64 + "\r\n"
        + pass_b64 + "\r\n",
        InboundAuthPolicy::ON);
    fx.run_on_io(h);

    auto* db = fx.db_pool->mock_conn().get();
    assert(wait_until([&] {
        return db->has_pending_query() &&
               !db->last_query_params().empty() && db->last_query_params()[0] == "bcuser@test.local";
    }));
    db->fire_query(std::make_shared<test::MockDbResult>(
        std::vector<std::map<std::string, std::string>>{{{"status", "1"}, {"password", stored_bcrypt}}}));

    assert(wait_until([&] { return db->has_pending_execute(); }));
    assert(!HAS(db->last_query_sql(), "SET password"));
    assert(HAS(db->last_query_sql(), "last_login_time"));
    db->fire_execute(true);

    assert(wait_until([&] { return HAS(h->captured, "235 Authentication successful"); }));

    AuthCacheEntry ce;
    assert(fx.fsm->m_authCache->lookup("bcuser@test.local", ce));
    assert(ce.password_hash == stored_bcrypt);   // 原样，没被重哈希
    std::cout << "  [PASS] auth_user_db_async_bcrypt_no_rehash (bcrypt 行: 不发密码 UPDATE, 缓存原样)" << std::endl;
}

// 脏 status（列为 NULL 时 get_value 给 ""；或 schema 漂移成非数字）必须 535，不许炸进程。
// 修复前：std::stoi("") 抛 invalid_argument 穿出 DB 回调——本测试里回调在 fire_query 的
// 调用线程执行，进程直接 abort（ctest 报 "Subprocess aborted"）；线上同一路径在
// boost::asio::thread_pool 的 worker 里，无人捕获 → std::terminate 整个 smtpsServer，
// 一行脏数据干掉所有在线连接。修复后 safe_stoull → 0 → 按"禁用"拒绝（fail-closed）。
TEST(auth_user_db_dirty_status) {
    fx.apply_config(fx.with_spf("off"));
    fx.db_pool->mock_conn()->set_deferred(true);
    fx.db_pool->mock_conn()->clear_pending();

    std::string user_plain = "dirty@test.local";
    std::string user_b64 = mail_system::outbound::base64_encode(
        reinterpret_cast<const unsigned char*>(user_plain.data()), user_plain.size());
    std::string pass_plain = "test123";
    std::string pass_b64 = mail_system::outbound::base64_encode(
        reinterpret_cast<const unsigned char*>(pass_plain.data()), pass_plain.size());

    auto h = fx.make_session(
        "EHLO test\r\n"
        "AUTH LOGIN\r\n"
        + user_b64 + "\r\n"
        + pass_b64 + "\r\n",
        InboundAuthPolicy::ON);
    fx.run_on_io(h);

    auto* db = fx.db_pool->mock_conn().get();
    assert(wait_until([&] {
        return db->has_pending_query() &&
               !db->last_query_params().empty() && db->last_query_params()[0] == "dirty@test.local";
    }));
    // status 为空串（NULL 列）、密码本身是对的 —— 结果必须只取决于 status
    db->fire_query(std::make_shared<test::MockDbResult>(
        std::vector<std::map<std::string, std::string>>{{{"status", ""}, {"password", "test123"}}}));

    assert(wait_until([&] { return HAS(h->captured, "535 Authentication failed"); }));
    // 没走到验密之后的任何 DB 写（不 rehash、不记 last_login）
    assert(!db->has_pending_execute());
    // 注：status!=1 的行在 store 之前就 return，不进认证缓存（三协议既有行为），
    // 所以这里不断言负缓存；每次尝试都会再查一次库，由 record_auth_failure 的次数上限兜底。
    std::cout << "  [PASS] auth_user_db_dirty_status (status=\"\" → 535, 进程存活, 无后续 DB 写)" << std::endl;
}

// ========== 3. DATA_END verify_all_from_file 异步路径 ==========

TEST(verify_all_from_file_async) {
    inbound::InboundVerifier::clear_dns_cache();
    ServerConfig c = fx.cfg;
    c.inbound_spf_mode   = "off";    // MAIL FROM 不查 SPF → spf_checked=false
    c.inbound_dkim_mode  = "off";
    c.inbound_dmarc_mode = "on";     // DATA_END 触发 verify_all_from_file → DMARC TXT 异步
    fx.apply_config(c);
    // DB mock 转同步：DATA_END 入队后后台持久化链立即完成，避免 worker 线程卡在未触发回调
    fx.db_pool->mock_conn()->set_deferred(false);
    fx.dns->set_txt("_dmarc.dmarcdom.com", {"v=DMARC1; p=none"});

    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<s@test.local>\r\n"
        "RCPT TO:<rcpt@test.local>\r\n"
        "DATA\r\n"
        "From: sender@dmarcdom.com\r\n"
        "To: rcpt@test.local\r\n"
        "Subject: verify async\r\n"
        "\r\n"
        "hello verify\r\n"
        ".\r\n");
    fx.run_on_io(h);

    // DATA_END 已暂停，等待 DMARC TXT 回调
    assert(wait_until([&] { return fx.dns->has_pending_txt("_dmarc.dmarcdom.com"); }));
    fx.dns->fire_txt("_dmarc.dmarcdom.com");

    // DMARC none → 校验完成 → 入队 + AFTER_ENQUEUE 响应 "250 OK"
    assert(wait_until([&] { return HAS(h->captured, "250 OK"); }));
    // 等待持久化 worker 处理完入队邮件，避免其后台查询干扰后续测试的 DB mock 状态
    assert(wait_until([&] { return fx.persist_q->inflight_count() == 0; }, 5000));
    std::cout << "  [PASS] verify_all_from_file_async (DMARC DNS 回调 → 入队 + 250 OK)" << std::endl;
}

TEST(verify_all_from_file_dkim_async) {
    inbound::InboundVerifier::clear_dns_cache();
    ServerConfig c = fx.cfg;
    c.inbound_spf_mode   = "off";    // MAIL FROM 不查 SPF
    c.inbound_dkim_mode  = "on";     // DATA_END 走 DKIM 异步 DNS（_domainkey TXT）
    c.inbound_dmarc_mode = "off";
    c.inbound_mime_parse_limit_bytes = 1;   // 跳过 eager MIME 解析，聚焦 DKIM 异步链
    fx.apply_config(c);
    fx.db_pool->mock_conn()->set_deferred(false);   // 入队后持久化链同步完成
    fx.dns->set_txt("s201512._domainkey.qq.com",
        {"v=DKIM1; k=rsa; p=MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDPsFIOSteMStsN615gUWK2RpNJ"
         "/B/ekmm4jVlu2fNzXADFkjF8mCMgh0uYe8w46FVqxUS97habZq6P5jmCj/WvtPGZ"
         "AX49jmdaB38hzZ5cUmwYZkdue6dM17sWocPZO8e7HVdq7bQwfGuUjVuMKfeTB3i"
         "Neo6/hFhb9TmUgnwjpQIDAQAB"});

    // 真实 QQ 邮件 fixture（有效 RSA-SHA256 签名，d=qq.com s=s201512）
    const std::string dkim_headers =
        "DKIM-Signature: v=1; a=rsa-sha256; c=relaxed/relaxed; d=qq.com; s=s201512;\r\n"
        "\tt=1783353315; bh=SFMxKRJds/1H9Vt0wd2tUM7QaMLL1sR7/mbKAGx4Jy4=;\r\n"
        "\th=From:To:Subject:Date;\r\n"
        "\tb=uGVkjlFb0ckSVEqYgpUGCQIfM4UjVj8Q3wvytvUwku4Egcfz1kiT7aF03OIomBUIv\r\n"
        "\t SVmQRpZRmkRD6857P85DEcdLfhR2DQ1tq09Evvzx7x6SgrkMDAAxA2Jd213cP48Sp2\r\n"
        "\t Ao6gyOhmg3kqHF4Mbe4pdBUGBUigc6Jog3rLlYbs=\r\n"
        "From: \"=?utf-8?B?Oi0p6Zu377yIICfilr8gJyDvvInnpZ4=?=\" <2466245103@qq.com>\r\n"
        "To: \"=?utf-8?B?cXQ=?=\" <qt@scut.email>\r\n"
        "Subject: =?utf-8?B?5rWL6K+V?=\r\n"
        "Date: Mon, 6 Jul 2026 23:55:14 +0800\r\n";
    const std::string dkim_body =
        "This is a multi-part message in MIME format.\r\n"
        "\r\n"
        "------=_NextPart_6A4BCFE2_D3382C80_59309AE2\r\n"
        "Content-Type: text/plain;\r\n"
        "\tcharset=\"utf-8\"\r\n"
        "Content-Transfer-Encoding: base64\r\n"
        "\r\n"
        "UVENCg0KDQoNCuWPkeiHquaIkeeahGlQaG9uZQ==\r\n"
        "\r\n"
        "------=_NextPart_6A4BCFE2_D3382C80_59309AE2\r\n"
        "Content-Type: text/html;\r\n"
        "\tcharset=\"utf-8\"\r\n"
        "Content-Transfer-Encoding: base64\r\n"
        "\r\n"
        "PGRpdiBzdHlsZT0ibWluLWhlaWdodDoyMnB4O21hcmdpbi1ib3R0b206OHB4OyI+UVE8L2Rp\r\n"
        "dj48ZGl2IHN0eWxlPSJtaW4taGVpZ2h0OjIycHg7bWFyZ2luLWJvdHRvbTo4cHg7Ij48YnIg\r\n"
        "IC8+PC9kaXY+PGRpdiBpZD0iUVFNYWlsU2lnbmF0dXJlIiBjbGFzcz0ibWFpbC1mb290ZXIi\r\n"
        "IGFyaWEtaGlkZGVuPSJ0cnVlIj48aHIgc3R5bGU9Im1hcmdpbjogMCAwIDEwcHggMDtib3Jk\r\n"
        "ZXI6IDA7Ym9yZGVyLWJvdHRvbToxcHggc29saWQgI0U2RThFQjtoZWlnaHQ6MDtsaW5lLWhl\r\n"
        "aWdodDowO2ZvbnQtc2l6ZTowO3BhZGRpbmc6IDIwcHggMCAwIDA7d2lkdGg6IDUwcHg7IiAg\r\n"
        "Lz7lj5Hoh6rmiJHnmoRpUGhvbmU8L2Rpdj48ZGl2IGlkPSJvcmlnaW5hbC1jb250ZW50Ij48\r\n"
        "L2Rpdj4=\r\n"
        "\r\n"
        "------=_NextPart_6A4BCFE2_D3382C80_59309AE2--";
    const std::string msg = dkim_headers + "\r\n" + dkim_body + "\r\n.\r\n";

    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<s@test.local>\r\n"
        "RCPT TO:<rcpt@test.local>\r\n"
        "DATA\r\n" + msg);
    fx.run_on_io(h);

    // DATA_END 已暂停，等待 DKIM 公钥 TXT 回调（证明 DKIM 异步路径被触发）
    assert(wait_until([&] { return fx.dns->has_pending_txt("s201512._domainkey.qq.com"); }));
    fx.dns->fire_txt("s201512._domainkey.qq.com");

    // DKIM 校验完成 → 入队 + AFTER_ENQUEUE 响应 "250 OK"
    assert(wait_until([&] { return HAS(h->captured, "250 OK"); }));
    assert(wait_until([&] { return fx.persist_q->inflight_count() == 0; }, 5000));
    std::cout << "  [PASS] verify_all_from_file_dkim_async (DKIM 异步 DNS 回调 → 入队 + 250 OK)" << std::endl;
}

// ========== 4. 关键边界：回调晚于 session 逻辑结束 → shared_ptr 保活不悬垂 ==========

TEST(boundary_callback_after_session_release) {
    inbound::InboundVerifier::clear_dns_cache();
    fx.apply_config(fx.with_spf("hard"));
    fx.dns->set_txt("attacker.com", {"v=spf1 -all"});

    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<spam@attacker.com>\r\n");
    fx.run_on_io(h);
    assert(wait_until([&] { return fx.dns->has_pending_txt("attacker.com"); }));

    // 模拟 io_context 即将关闭 / 连接已逻辑结束：测试释放唯一外部 session 引用。
    // 此时 session 唯一存活来源是 DNS 回调链内部持有的 shared_ptr。
    std::weak_ptr<SmtpsSession<MockConnection>> weak = h->session;
    h->session.reset();
    assert(weak.expired() == false);   // 回调链仍持有 shared_ptr → session 存活

    // 触发回调：应能在 session 上完成状态机操作而不悬垂
    fx.dns->fire_txt("attacker.com");

    // 回调完成后所有 shared_ptr 释放 → session 干净析构
    assert(wait_until([&] { return weak.expired(); }));
    assert(wait_until([&] { return HAS(h->captured, "550 5.7.1 SPF verification failed"); }));   // 回调期间完成写响应
    std::cout << "  [PASS] boundary_callback_after_session_release (shared_ptr 保活，无悬垂)" << std::endl;
}

// ========== 5. 跨线程回调：Manual 触发 + 独立线程投递（TSan 下验证无数据竞争） ==========
//
// 与生产时序一致：io 线程已完成命令链并暂停（run_on_io 内 wait_idle），
// 然后 DNS 回调在独立线程触发，续跑 FSM（跨线程但不与 io 线程并发访问 session）。
// 避免 AutoDelay 固定小延迟在 io 线程尚未退出命令链时过早触发（生产 DNS 延迟
// 远大于 io 线程退出耗时，过早触发是测试假象）。MockDnsResolver::AutoDelay 模式
// 保留供压力测试：需用足够大的 delay 匹配生产时序。

TEST(cross_thread_callback_background_fire) {
    inbound::InboundVerifier::clear_dns_cache();
    fx.apply_config(fx.with_spf("pass"));
    fx.dns->set_txt("sender.com", {"v=spf1 ip4:127.0.0.1"});   // 客户端 IP=127.0.0.1 → pass

    const int kIterations = 8;
    std::vector<std::shared_ptr<SessionHandle>> handles;
    for (int i = 0; i < kIterations; ++i) {
        handles.push_back(fx.make_session(
            "EHLO test\r\n"
            "MAIL FROM:<s@sender.com>\r\n"
            "RCPT TO:<rcpt@test.local>\r\n"));
    }

    // 并发发起多个 session：命令链在各自 MockIoContext 线程执行并暂停于 SPF
    std::vector<std::thread> starters;
    for (auto& h : handles)
        starters.emplace_back([&fx, h]() { fx.run_on_io(h); });
    for (auto& t : starters) t.join();
    for (auto& h : handles)
        assert(wait_until([&] { return fx.dns->has_pending_txt("sender.com"); }));

    // 在独立线程触发 DNS 回调（模拟 c-ares 线程），跨线程续跑 FSM → 250 Ok
    std::vector<std::thread> firers;
    for (auto& h : handles)
        firers.emplace_back([&fx]() { fx.dns->fire_txt("sender.com"); });
    for (auto& t : firers) t.join();

    // 等待全部 session 完成 → 250 Ok（随后无输入 → eof → close）
    // 用 written()（内部加锁）轮询，避免与回调线程写 captured 并发读竞态
    bool all_done = wait_until([&] {
        for (auto& h : handles)
            if (!HAS(h->conn->written(), "250 Ok")) return false;
        return true;
    }, 5000);
    assert(all_done);

    std::cout << "  [PASS] cross_thread_callback_background_fire (" << kIterations
              << " sessions, 回调跨线程触发, TSan 下无 data race)" << std::endl;
}

// ========== main ==========
int main() {
    std::cout << "Inbound SMTP Async/Concurrency Test Suite\n===========================================\n";

    try {
        ConcurrencyTestFixture fx;

        // 1. SPF 异步
        test_spf_hard_fail_async(fx);
        test_spf_pass_async(fx);

        // 2. RCPT user_exists DB 异步
        test_user_exists_db_async_exists(fx);
        test_user_exists_db_async_not_found(fx);

        // 2b. AUTH auth_user_async DB 异步（明文行 rehash 链 + bcrypt 行对照 + 脏 status 不炸）
        test_auth_user_db_async(fx);
        test_auth_user_db_async_bcrypt_no_rehash(fx);
        test_auth_user_db_dirty_status(fx);

        // 3. DATA_END verify_all_from_file 异步
        test_verify_all_from_file_async(fx);
        test_verify_all_from_file_dkim_async(fx);

        // 4. 边界：回调晚于 session 结束
        test_boundary_callback_after_session_release(fx);

        // 5. 跨线程回调（独立线程触发）
        test_cross_thread_callback_background_fire(fx);

        std::cout << "\nAll concurrency tests passed.\n";
    } catch (const std::exception& e) {
        std::cerr << "FATAL: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
