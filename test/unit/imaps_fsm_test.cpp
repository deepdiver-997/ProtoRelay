#include "test_temp_dir.h"
// IMAP FSM 单元测试 — MockConnection 零 I/O 验证状态转换和响应码
// 2026-10-18：手机同时 FETCH 多个 BODY section 仅收到第一段；见
// docs/bugfixes/2026-10-18-imap-multiple-body-sections.md。
#undef NDEBUG
#include "mail_system/back/storage/mail_object_key_layout.h"
#include "no_mail_layout_storage.h"
#include <cassert>
#include <chrono>
#include <fstream>
#include <functional>
#include <filesystem>
#include <cstdlib>
#include "mail_system/back/mailServer/imaps_server.h"
#include "mail_system/back/mailServer/fsm/imaps/traditional_imaps_fsm.tpp"
#include "mail_system/back/mailServer/session/imaps_session.tpp"
#include "framework/session_base.tpp"
#include "framework/server_config.h"
#include "framework/thread_pool/io_thread_pool.h"
#include "framework/thread_pool/worker_thread_pool.h"
#include "mail_system/back/router/static_shard_router.h"
#include "mail_system/back/common/logger.h"
#include "mail_system/back/common/mail_crypto.h"
#include "mock_connection.h"
#include "mock_db_pool.h"
#include "mail_system/back/storage/local_file_storage_provider.h"

#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <atomic>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

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
    std::shared_ptr<pr::IOThreadPool> io_pool;
    std::shared_ptr<pr::WorkerThreadPool> worker_pool;
    std::shared_ptr<router::StaticShardRouter> router;
    std::shared_ptr<TestServer> server;
    std::shared_ptr<TraditionalImapsFsm<MockConnection>> fsm;
    ServerConfig cfg;

    FsmTestFixture() {
        Logger::get_instance().init("imaps_fsm_test.log", 0, 1, pr::LogLevel::off, false, false);
        io_pool     = std::make_shared<pr::IOThreadPool>(1);
        worker_pool = std::make_shared<pr::WorkerThreadPool>(2);
        io_pool->start();
        worker_pool->start();

        router = std::make_shared<router::StaticShardRouter>(
            std::vector<std::pair<std::string, int>>{},
            0,
            std::vector<std::shared_ptr<DBPool>>{},
            std::vector<std::shared_ptr<storage::IStorageProvider>>{});

        cfg.perf_mode     = true;
        cfg.apply_perf_mode();
        cfg.use_database   = false;
        cfg.system_domain  = "test.local";
        cfg.storage.local.mail_path       = test_support::temp_path("imaps_fsm_test_mail");
        cfg.storage.local.attachment_path = test_support::temp_path("imaps_fsm_test_att");
        std::filesystem::create_directories(test_support::temp_path("imaps_fsm_test_mail")); std::filesystem::create_directories(test_support::temp_path("imaps_fsm_test_att"));

        server = std::shared_ptr<TestServer>(new TestServer(cfg, io_pool, worker_pool, router));

        fsm = std::make_shared<TraditionalImapsFsm<MockConnection>>(
            io_pool, worker_pool, router);

        // 预注入 auth 缓存
        for (int i = 0; i < 3; ++i) {
            std::string email = "user" + std::to_string(i) + "@test.local";
            AuthCacheEntry entry;
            entry.password_hash = "test123";
            entry.status = 1;
            fsm->m_authCache->inject(email, entry);
        }
    }

    ~FsmTestFixture() {
        worker_pool->stop();
        io_pool->stop();
        std::filesystem::remove_all(test_support::temp_path("imaps_fsm_test_mail")); std::filesystem::remove_all(test_support::temp_path("imaps_fsm_test_att"));
    }

    struct Handle {
        MockConnection* conn;
        std::shared_ptr<ImapsSession<MockConnection>> session;

        // 收尾用 finish_session（close + 排空 + 等引用收敛）。IMAP 侧有两类
        // 强引用必须在这里断掉，否则一律 LSan indirect leak：
        //   1) do_async_*/close 的发起 post 捕获 self，留在 exec_ctx_ 队列即成环；
        //   2) watchdog 的 async_wait handler 强持 self，只有 close() 会 disarm。
        ~Handle() { finish_session(session); }
    };

    Handle make_session(const std::string& preload_data = "") {
        auto conn_u = std::make_unique<MockConnection>();
        auto* conn_ptr = conn_u.get();
        if (!preload_data.empty())
            conn_ptr->set_read_data(preload_data);

        auto session = std::make_shared<ImapsSession<MockConnection>>(
            server.get(), std::move(conn_u),
            fsm);

        return {conn_ptr, session};
    }
};

#define TEST(name) void test_##name(FsmTestFixture& fx)
#define HAS(str, sub) ((str).find(sub) != std::string::npos)

TEST(connection_lease_keeps_pool_after_fsm_destruction) {
    struct TrackingPool final : test::MockDbPool {
        std::shared_ptr<std::vector<std::string>> events;
        explicit TrackingPool(std::shared_ptr<std::vector<std::string>> value) : events(std::move(value)) {}
        ~TrackingPool() override { events->push_back("destroy"); }
        void release_connection(std::shared_ptr<pr::IDBConnection>) override { events->push_back("return"); }
    };
    auto events = std::make_shared<std::vector<std::string>>();
    auto pool = std::make_shared<TrackingPool>(events);
    auto db = pool->mock_conn();
    std::weak_ptr<TrackingPool> weak = pool;
    auto router = std::make_shared<router::StaticShardRouter>(
        std::vector<std::pair<std::string, int>>{}, 0,
        std::vector<std::shared_ptr<DBPool>>{pool},
        std::vector<std::shared_ptr<storage::IStorageProvider>>{});
    auto fsm = std::make_shared<TraditionalImapsFsm<MockConnection>>(nullptr, nullptr, router);
    auto lease = fsm->acquire_connection(0);
    bool complete = false;
    fsm->get_message_repository()->get_mail_async(lease, 81,
        [&](bool ok, MailboxMailInfo value) {
            assert(ok && value.mail_id == 81 && !weak.expired()); complete = true;
        });
    lease.reset(); fsm.reset(); router.reset(); pool.reset();
    assert(!weak.expired() && events->empty());
    db->fire_query(std::make_shared<test::MockDbResult>(
        std::vector<std::map<std::string, std::string>>{{{"id", "81"}, {"body_path", "opaque/81"}}}));
    assert(complete && weak.expired());
    assert((*events == std::vector<std::string>{"return", "destroy"}));
    std::cout << "  [PASS] connection_lease_keeps_pool_after_fsm_destruction" << std::endl;
}

// ========== 基础命令 ==========

TEST(capability_response) {
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::CAPABILITY, "A001");
    h.conn->pump_executor();
    auto w = h.conn->written();
    assert(HAS(w, "CAPABILITY IMAP4rev1"));
    assert(HAS(w, "AUTH=PLAIN"));   // SASL PLAIN 通告（2026-09-28 起）
    std::cout << "  [PASS] capability_response" << std::endl;
}

TEST(noop_reply) {
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    ctx->current_tag = "C001";
    fx.fsm->process_event(h.session, ImapEvent::NOOP, "C001");
    h.conn->pump_executor();
    auto w = h.conn->written();
    assert(HAS(w, "C001 OK NOOP completed"));
    std::cout << "  [PASS] noop_reply" << std::endl;
}

// LOGIN 已异步化（bcrypt 在 worker 线程）：回复可能晚于 process_event 返回，
// 轮询等待而不是立刻断言。
template <typename Handle>
static std::string wait_for_reply(Handle& h, int timeout_ms = 2000) {
    for (int waited = 0; waited < timeout_ms; waited += 5) {
        h.conn->pump_executor();   // 异步回复落在 exec_ctx_ 队列,轮询需排空
        auto w = h.conn->written();
        if (!w.empty()) return w;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    h.conn->pump_executor();
    return h.conn->written();
}

TEST(login_response) {
    // 直接调用 process_event 测试 FSM handler（不依赖 command parser）
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::LOGIN, "A001");
    h.conn->pump_executor();
    auto w = wait_for_reply(h);
    // handler 会尝试从 last_command_args 解析凭据，可能 OK 或 NO
    assert(!w.empty());
    std::cout << "  [PASS] login_response" << std::endl;
}

TEST(login_success_real_auth) {
    // 剧情：真实 parser 驱动 LOGIN → worker 异步 auth（fixture auth cache 命中
    // user0@test.local/test123）→ tagged OK。旧测试只断言 !empty，抓不住
    // "auth 收到空用户名"这类参数在回调构造时被 std::move 走的回归。
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));
    h.session->handle_read("A001 LOGIN user0@test.local test123\r\n");
    h.session->process_read();
    auto w = wait_for_reply(h);
    assert(w.find("A001 OK LOGIN completed") != std::string::npos);
    std::cout << "  [PASS] login_success_real_auth (real parser → async auth)" << std::endl;
}

TEST(login_uses_injected_authentication_service) {
    struct Authenticator final : IAccountAuthenticator {
        std::atomic<int> calls{0};
        void login_async(const std::string& address, const std::string&, Complete complete, bool) override {
            ++calls;
            AccountAuthResult result;
            result.ok = true;
            result.address = address;
            result.user_id = 202;
            complete(std::move(result));
        }
    };
    auto authenticator = std::make_shared<Authenticator>();
    fx.fsm->set_authenticator(authenticator);
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));
    // This account is absent from both the cache and database. The frontend
    // accepts the injected application's result without performing credential SQL.
    h.session->handle_read("I001 LOGIN injected@test.local ignored\r\n");
    h.session->process_read();
    auto reply = wait_for_reply(h);
    assert(HAS(reply, "I001 OK LOGIN completed"));
    assert(authenticator->calls == 1);
    auto* context = static_cast<ImapContext*>(h.session->get_context());
    assert(context->is_authenticated && context->user_id == 202);
    fx.fsm->set_authenticator(std::make_shared<AccountAuthenticationService>(
        fx.router, fx.fsm->m_authCache, fx.worker_pool));
    std::cout << "  [PASS] login_uses_injected_authentication_service" << std::endl;
}

TEST(login_wrong_password) {
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::LOGIN, "A002");
    h.conn->pump_executor();
    auto w = wait_for_reply(h);
    assert(!w.empty());
    std::cout << "  [PASS] login_wrong_password" << std::endl;
}

// ========== AUTHENTICATE PLAIN ==========
// PLAIN 消息 = authzid NUL authcid NUL passwd
static std::string sasl_plain_b64(const std::string& authcid, const std::string& password) {
    std::string plain;
    plain += '\0';
    plain += authcid;
    plain += '\0';
    plain += password;
    return outbound::base64_encode(reinterpret_cast<const unsigned char*>(plain.data()),
                                   plain.size());
}

// 异步回复轮询：等到 written 出现 needle（或超时），避免 "+ " 抢先满足 wait_for_reply
template <typename Handle>
static std::string poll_written_for(Handle& h, const std::string& needle, int timeout_ms = 2000) {
    for (int waited = 0; waited < timeout_ms; waited += 5) {
        h.conn->pump_executor();
        if (h.conn->written().find(needle) != std::string::npos) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    h.conn->pump_executor();
    return h.conn->written();
}

TEST(abnormal_trace_after_auth) {
    namespace fs = std::filesystem;
    const auto root = test_support::temp_root() /
        ("imaps-trace-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto cfg = fx.cfg;
    cfg.log_file = (root / "imap.log").string();
    auto server = std::make_shared<TestServer>(cfg, fx.io_pool, fx.worker_pool, fx.router);
    const auto b64 = sasl_plain_b64("user0@test.local", "test123");
    {
        auto conn = std::make_unique<MockConnection>();
        conn->set_deferred_read(true);
        auto* raw = conn.get();
        auto session = std::make_shared<ImapsSession<MockConnection>>(server.get(), std::move(conn), fx.fsm);
        FsmTestFixture::Handle h{raw, session};
        session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));
        const std::string login = "T1 LOGIN user0@test.local test123\r\n";
        session->trace_append_inbound(login);
        session->handle_read(login);
        session->process_read();
        assert(HAS(poll_written_for(h, "T1 OK LOGIN completed"), "T1 OK LOGIN completed"));
        session->trace_append_inbound("T2 AUTHENTICATE PLAIN " + b64 + "\r\n");
        auto* ctx = static_cast<ImapContext*>(session->get_context());
        ctx->sasl_plain_state = 1;
        session->trace_append_inbound(b64 + "\r\n");
        ctx->sasl_plain_state = 0;
        session->trace_append_inbound("T3 UID FETCH 43 (BODY.PEEK[2] BODY.PEEK[1])\r\n");
        session->close(); // 已认证但未 LOGOUT，必须留取信阶段的对话
    }
    std::vector<fs::path> traces;
    for (const auto& file : fs::directory_iterator(root / "traces")) traces.push_back(file.path());
    assert(traces.size() == 1);
    std::ifstream in(traces.front(), std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    const bool redacted = !HAS(text, "test123") && !HAS(text, b64);
    const bool auth = HAS(text, "auth: yes");
    const bool private_file = (fs::status(traces.front()).permissions() &
        (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none;
    std::cout << "  trace credentials_redacted=" << redacted << " auth_correct=" << auth
              << " file_private=" << private_file << std::endl;
    assert(redacted && auth && private_file);
    assert(HAS(text, "T3 UID FETCH 43 (BODY.PEEK[2] BODY.PEEK[1])"));
    {
        auto session = std::make_shared<ImapsSession<MockConnection>>(
            server.get(), std::make_unique<MockConnection>(), fx.fsm);
        session->trace_append_inbound("T4 LOGOUT\r\n");
        session->set_trace_clean_close();
        finish_session(session);
    }
    size_t count = 0;
    for (const auto& file : fs::directory_iterator(root / "traces")) { (void)file; ++count; }
    assert(count == 1); // 正常退出不新增文件
    fs::remove_all(root);
    std::cout << "  [PASS] abnormal_trace_after_auth" << std::endl;
}

TEST(auth_plain_ir_success) {
    // SASL-IR：初始响应随命令行（fixture auth cache 命中 user0@test.local/test123）
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));
    h.session->handle_read("A001 AUTHENTICATE PLAIN " + sasl_plain_b64("user0@test.local", "test123") + "\r\n");
    h.session->process_read();
    auto w = poll_written_for(h, "A001 OK AUTHENTICATE completed");
    assert(w.find("A001 OK AUTHENTICATE completed") != std::string::npos);
    std::cout << "  [PASS] auth_plain_ir_success" << std::endl;
}

TEST(auth_plain_two_step) {
    // 无初始响应：服务器发空挑战 "+ "，客户端续行 base64。
    // 续行要先进 mock 读队列："+ " 写完成后 session 会 re-arm 读，mock 无数据
    // 即 EOF 关连接（生产上客户端此时正在发续行，不存在这个窗口）。
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));
    h.session->handle_read("A001 AUTHENTICATE PLAIN\r\n");
    h.conn->append_read_data(sasl_plain_b64("user0@test.local", "test123") + "\r\n");
    h.session->process_read();
    auto w = poll_written_for(h, "A001 OK AUTHENTICATE completed");
    assert(w.find("A001 OK AUTHENTICATE completed") != std::string::npos);
    std::cout << "  [PASS] auth_plain_two_step" << std::endl;
}

TEST(auth_plain_abort) {
    // RFC 3501: 续行位置发 "*" 中止（同样先把续行喂进 mock 读队列）
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));
    h.session->handle_read("A001 AUTHENTICATE PLAIN\r\n");
    h.conn->append_read_data("*\r\n");
    h.session->process_read();
    auto w = poll_written_for(h, "A001 NO AUTHENTICATE aborted");
    assert(w.find("A001 NO AUTHENTICATE aborted") != std::string::npos);
    std::cout << "  [PASS] auth_plain_abort" << std::endl;
}

TEST(auth_plain_wrong_password) {
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));
    h.session->handle_read("A001 AUTHENTICATE PLAIN " + sasl_plain_b64("user0@test.local", "wrong") + "\r\n");
    h.session->process_read();
    auto w = poll_written_for(h, "A001 NO AUTHENTICATE failed");
    assert(w.find("A001 NO AUTHENTICATE failed") != std::string::npos);
    std::cout << "  [PASS] auth_plain_wrong_password" << std::endl;
}

TEST(auth_plain_bad_base64) {
    // 解码失败 / 缺 NUL 分隔 → NO
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));
    h.session->handle_read("A001 AUTHENTICATE PLAIN not-base64!!\r\n");
    h.session->process_read();
    auto w = poll_written_for(h, "A001 NO AUTHENTICATE failed");
    assert(w.find("A001 NO AUTHENTICATE failed") != std::string::npos);
    std::cout << "  [PASS] auth_plain_bad_base64" << std::endl;
}

TEST(auth_unsupported_mechanism) {
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));
    h.session->handle_read("A001 AUTHENTICATE CRAM-MD5\r\n");
    h.session->process_read();
    h.conn->pump_executor();
    assert(HAS(h.conn->written(), "A001 NO Unsupported authentication mechanism: CRAM-MD5"));
    std::cout << "  [PASS] auth_unsupported_mechanism" << std::endl;
}

TEST(login_many_failures_close) {
    // 多次失败 LOGIN → 达到 max_auth_attempts（测试配置=2）后连接被关闭
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));

    // LOGIN 已异步化（bcrypt 在 worker 线程池）。直接连发多个 process_event 会让
    // 多个 worker 线程并发处理同一 session → 并发改写 session 的 std::string
    // （command_read_buffer_/last_command_args_/m_trace_buf），Linux libstdc++ 下
    // 表现为 heap-use-after-free（macOS libc++ 不崩，CI 才暴露）。
    // 生产流水线靠 drain+暂停串行化，但测试直接调 process_event 绕过了它，
    // 必须逐个等待该次认证完成（回复 NO / 连接关闭）再发下一个。
    size_t total_no = 0;
    for (const char* tag : {"A001", "A002", "A003"}) {
        h.conn->clear_written();   // 只统计本次回复，避免匹配到上次的过期 NO
        fx.fsm->process_event(h.session, ImapEvent::LOGIN, tag);
        h.conn->pump_executor();
        for (int waited = 0; waited < 2000; waited += 5) {
            auto w = h.conn->written();
            if (HAS(w, "NO LOGIN failed")) { ++total_no; break; }   // 该次失败已回复
            if (!h.conn->is_open()) break;                          // 失败数达上限 → 关闭
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!h.conn->is_open()) break;   // 连接已关闭，无需再发
    }
    assert(total_no >= 1 || !h.conn->is_open());
    std::cout << "  [PASS] login_many_failures_close (NO count=" << total_no << ")" << std::endl;
}

TEST(logout_bye) {
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    ctx->current_tag = "B001";
    fx.fsm->process_event(h.session, ImapEvent::LOGOUT, "B001");
    h.conn->pump_executor();
    auto w = h.conn->written();
    assert(HAS(w, "BYE IMAP4rev1 Server logging out"));
    assert(HAS(w, "B001 OK LOGOUT completed"));
    std::cout << "  [PASS] logout_bye" << std::endl;
}

// ========== 未认证状态拒绝 ==========

TEST(select_without_login) {
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::SELECT, "D001");
    h.conn->pump_executor();
    auto w = h.conn->written();
    assert(HAS(w, "D001 NO") || HAS(w, "BAD"));
    std::cout << "  [PASS] select_without_login" << std::endl;
}

TEST(fetch_without_login) {
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::FETCH, "E001");
    h.conn->pump_executor();
    auto w = h.conn->written();
    assert(HAS(w, "E001 NO") || HAS(w, "BAD"));
    std::cout << "  [PASS] fetch_without_login" << std::endl;
}

// ========== UID FETCH crash repro ==========

TEST(uid_fetch_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    ctx->user_id = 1;
    ctx->mailbox_selected = true;
    ctx->selected_mailbox_id = 1;
    ctx->current_tag = "A001";
    h.session->set_current_state(static_cast<int>(ImapState::SELECTED));

    // 直接调用 UID → FETCH handler（模拟客户端发 UID FETCH 1:* (FLAGS)）
    fx.fsm->process_event(h.session, ImapEvent::UID, "A001");
    h.conn->pump_executor();
    auto w = h.conn->written();
    std::cout << "  [PASS] uid_fetch_no_db response=[" << w.substr(0, 80) << "]" << std::endl;
}

// ========== 全路径 FETCH（续作链 + 存储读取） ==========
TEST(fetch_full_path_with_storage) {
    // 覆盖重写后的续作链：mock DB 提供邮件行 + LocalFileStorageProvider
    // 提供正文；本地 provider 的 async_* 内联执行，响应应同步产出。
    // 断言 size/正文/BODY 字面量/tagged OK —— 这是 FETCH 热路径的形状。
    auto io = std::make_shared<pr::IOThreadPool>(1); io->start();
    auto wk = std::make_shared<pr::WorkerThreadPool>(2); wk->start();
    auto db = std::make_shared<test::MockDbPool>();
    db->mock_conn()->set_deferred(false);   // 同步模式：sq 桥立即拿到注入的行
    namespace st = mail_system::storage;
    std::filesystem::create_directories(test_support::temp_path("imaps_fetch_test"));
    auto storage = std::make_shared<st::LocalFileStorageProvider>(
        test_support::temp_path("imaps_fetch_test/"), test_support::temp_path("imaps_fetch_test/"));
    auto router2 = std::make_shared<router::StaticShardRouter>(
        std::vector<std::pair<std::string, int>>{},
        0,
        std::vector<std::shared_ptr<DBPool>>{db},
        std::vector<std::shared_ptr<st::IStorageProvider>>{storage});

    ServerConfig cfg2;
    cfg2.perf_mode = true; cfg2.apply_perf_mode();
    cfg2.use_database = false; cfg2.system_domain = "test.local";
    cfg2.storage.local.mail_path = test_support::temp_path("imaps_fsm_test_mail2");
    cfg2.storage.local.attachment_path = test_support::temp_path("imaps_fsm_test_att2");
    auto server2 = std::shared_ptr<TestServer>(new TestServer(cfg2, io, wk, router2));
    auto fsm2 = std::make_shared<TraditionalImapsFsm<MockConnection>>(io, wk, router2);

    const std::string body = "Subject: hello\r\n\r\nworld body 123";
    const std::string body_path = test_support::temp_path("imaps_fetch_test/mail_42");
    { std::ofstream f(body_path, std::ios::binary); f << body; }

    db->mock_conn()->push_sync_result(std::make_shared<test::MockDbResult>(
        std::vector<std::map<std::string, std::string>>{{
            {"id", "42"}, {"sender", "a@t.local"}, {"recipient", "b@t.local"},
            {"subject", "hi"}, {"body_path", body_path},
            {"is_starred", "0"}, {"is_deleted", "0"}, {"is_important", "0"},
            {"status", "1"}, {"send_time", "1700000000"}}}));

    auto conn_u = std::make_unique<MockConnection>();
    auto* conn_ptr = conn_u.get();
    auto session = std::make_shared<ImapsSession<MockConnection>>(
        server2.get(), std::move(conn_u), fsm2);
    auto* c = static_cast<ImapContext*>(session->get_context());
    c->is_authenticated = true;
    c->user_id = 1;
    c->mailbox_selected = true;
    c->selected_mailbox_id = 1;
    c->current_tag = "A001";
    session->set_current_state(static_cast<int>(ImapState::SELECTED));

    // 走真实解析+分发路径：handle_read 解析 tag/cmd/args，process_read 派发 FSM
    session->handle_read("A001 FETCH 1 (FLAGS RFC822.SIZE BODY[])\r\n");
    session->process_read();

    // FETCH 链含 DB/存储异步段，响应还经 exec_ctx_ 串行化 → 轮询等完成标记
    for (int waited = 0; waited < 2000 && conn_ptr->written().find(
            "A001 OK FETCH completed") == std::string::npos; waited += 5) {
        conn_ptr->pump_executor();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    conn_ptr->pump_executor();
    auto w = conn_ptr->written();
    assert(w.find("RFC822.SIZE " + std::to_string(body.size())) != std::string::npos);
    assert(w.find("BODY[] {" + std::to_string(body.size()) + "}") != std::string::npos);
    assert(w.find("world body 123") != std::string::npos);
    assert(w.find("A001 OK FETCH completed") != std::string::npos);

    std::filesystem::remove_all(test_support::temp_path("imaps_fetch_test"));
    io->stop(); wk->stop();
    // 收尾：本用例不走 Handle。FETCH 链在会话里武装了 watchdog 定时器
    // （async_wait handler 强持 self），不 close+drain 就是一笔 LSan 泄漏。
    // 先把线程池停掉（不再有新续作投递），再收尾。
    finish_session(session);
    std::cout << "  [PASS] fetch_full_path_with_storage" << std::endl;
}

// ========== BODY.PEEK[n] 按部分取正文回归 ==========
// 89449fa 引入的 substr off-by-one 把闭合 ']' 带进节号（"2]"→非数字→节号恒 0），
// 服务器对 BODY[n] 一律不返回正文 → 客户端"content 为空"无限重试（2026-09-28）。
TEST(fetch_body_part_number) {
    auto io = std::make_shared<pr::IOThreadPool>(1); io->start();
    auto wk = std::make_shared<pr::WorkerThreadPool>(2); wk->start();
    auto db = std::make_shared<test::MockDbPool>();
    db->mock_conn()->set_deferred(false);
    namespace st = mail_system::storage;
    std::filesystem::create_directories(test_support::temp_path("imaps_fetch_part_test"));
    auto storage = std::make_shared<st::LocalFileStorageProvider>(
        test_support::temp_path("imaps_fetch_part_test/"), test_support::temp_path("imaps_fetch_part_test/"));
    auto router2 = std::make_shared<router::StaticShardRouter>(
        std::vector<std::pair<std::string, int>>{},
        0,
        std::vector<std::shared_ptr<DBPool>>{db},
        std::vector<std::shared_ptr<st::IStorageProvider>>{storage});

    ServerConfig cfg2;
    cfg2.perf_mode = true; cfg2.apply_perf_mode();
    cfg2.use_database = false; cfg2.system_domain = "test.local";
    cfg2.storage.local.mail_path = test_support::temp_path("imaps_fsm_test_mail3");
    cfg2.storage.local.attachment_path = test_support::temp_path("imaps_fsm_test_att3");
    auto server2 = std::shared_ptr<TestServer>(new TestServer(cfg2, io, wk, router2));
    auto fsm2 = std::make_shared<TraditionalImapsFsm<MockConnection>>(io, wk, router2);

    const std::string body =
        "Subject: mime\r\n"
        "Content-Type: multipart/alternative; boundary=\"BBBB\"\r\n"
        "\r\n"
        "--BBBB\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "hello plain part\r\n"
        "\r\n"
        "--BBBB\r\n"
        "Content-Type: text/html\r\n"
        "\r\n"
        "<p>hello html part</p>\r\n"
        "\r\n"
        "--BBBB--\r\n";
    const std::string body_path = test_support::temp_path("imaps_fetch_part_test/mail_43");
    { std::ofstream f(body_path, std::ios::binary); f << body; }

    db->mock_conn()->push_sync_result(std::make_shared<test::MockDbResult>(
        std::vector<std::map<std::string, std::string>>{{
            {"id", "43"}, {"sender", "a@t.local"}, {"recipient", "b@t.local"},
            {"subject", "mime"}, {"body_path", body_path},
            {"is_starred", "0"}, {"is_deleted", "0"}, {"is_important", "0"},
            {"status", "1"}, {"send_time", "1700000000"}}}));

    auto conn_u = std::make_unique<MockConnection>();
    auto* conn_ptr = conn_u.get();
    auto session = std::make_shared<ImapsSession<MockConnection>>(
        server2.get(), std::move(conn_u), fsm2);
    auto* c = static_cast<ImapContext*>(session->get_context());
    c->is_authenticated = true;
    c->user_id = 1;
    c->mailbox_selected = true;
    c->selected_mailbox_id = 1;
    c->current_tag = "A001";
    session->set_current_state(static_cast<int>(ImapState::SELECTED));

    // 异步 FETCH 完成后 mock 环境不会重新武装读路径，每个 session 只发一条命令
    auto fetch_and_wait = [&](ImapsSession<MockConnection>& s, MockConnection* conn,
                              const std::string& tag_line, const std::string& done_marker) {
        s.handle_read(tag_line);
        s.process_read();
        for (int waited = 0; waited < 2000 && conn->written().find(
                done_marker) == std::string::npos; waited += 5) {
            conn->pump_executor();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        conn->pump_executor();
        return conn->written();
    };

    auto w1 = fetch_and_wait(*session, conn_ptr, "A001 FETCH 1 (BODY.PEEK[1])\r\n",
                             "A001 OK FETCH completed");
    assert(w1.find("BODY[1] {16}") != std::string::npos);   // 节号解析成功且长度正确
    assert(w1.find("hello plain part") != std::string::npos);

    auto conn_u2 = std::make_unique<MockConnection>();
    auto* conn_ptr2 = conn_u2.get();
    auto session2 = std::make_shared<ImapsSession<MockConnection>>(
        server2.get(), std::move(conn_u2), fsm2);
    auto* c2 = static_cast<ImapContext*>(session2->get_context());
    c2->is_authenticated = true;
    c2->user_id = 1;
    c2->mailbox_selected = true;
    c2->selected_mailbox_id = 1;
    c2->current_tag = "A002";
    session2->set_current_state(static_cast<int>(ImapState::SELECTED));

    // mock DB 结果是队列：第二个 session 的邮箱查询需要再压一份同样的结果
    db->mock_conn()->push_sync_result(std::make_shared<test::MockDbResult>(
        std::vector<std::map<std::string, std::string>>{{
            {"id", "43"}, {"sender", "a@t.local"}, {"recipient", "b@t.local"},
            {"subject", "mime"}, {"body_path", body_path},
            {"is_starred", "0"}, {"is_deleted", "0"}, {"is_important", "0"},
            {"status", "1"}, {"send_time", "1700000000"}}}));

    auto w2 = fetch_and_wait(*session2, conn_ptr2,
                             "A002 FETCH 1 (BODY.PEEK[2] BODY.PEEK[1] BODY.PEEK[] BODYSTRUCTURE)\r\n",
                             "A002 OK FETCH completed");
    assert(w2.find("BODY[2] {") != std::string::npos);      // 节号 2 同样解析成功
    assert(w2.find("<p>hello html part</p>") != std::string::npos);

    // 消费者按 literal 的声明长度读取每段，不能只检查某个标签存在。
    auto literal = [&](const std::string& label) {
        auto start = w2.find(label + " {");
        assert(start != std::string::npos);
        start += label.size() + 2;
        auto end = w2.find("}\r\n", start);
        assert(end != std::string::npos);
        auto count = std::stoull(w2.substr(start, end - start));
        assert(end + 3 + count <= w2.size());
        return w2.substr(end + 3, count);
    };
    assert(literal("BODY[2]") == "<p>hello html part</p>");
    assert(literal("BODY[1]") == "hello plain part");
    assert(literal("BODY[]") == body);
    assert(w2.find("BODY[2]") < w2.find("BODY[1]"));
    assert(w2.find("BODY[1]") < w2.find("BODY[]"));
    assert(w2.find("BODYSTRUCTURE ((") != std::string::npos);

    std::filesystem::remove_all(test_support::temp_path("imaps_fetch_part_test"));
    io->stop(); wk->stop();
    finish_session(session);
    finish_session(session2);
    std::cout << "  [PASS] fetch_body_part_number" << std::endl;
}

// FETCH 续作链迭代化回归：400 封。本地 storage 的 async_* 回调内联触发，
// 旧实现 fetch_drive 逐封真递归 → 大邮箱 >~200 封栈溢出 SIGSEGV。
// 修复后应循环展开（inline 回调外层 continue），400 封正常完成。
TEST(fetch_many_mails_no_stack_overflow) {
    auto io = std::make_shared<pr::IOThreadPool>(1); io->start();
    auto wk = std::make_shared<pr::WorkerThreadPool>(2); wk->start();
    auto db = std::make_shared<test::MockDbPool>();
    db->mock_conn()->set_deferred(false);
    namespace st = mail_system::storage;
    std::filesystem::create_directories(test_support::temp_path("imaps_fetch_many"));
    auto storage = std::make_shared<st::LocalFileStorageProvider>(
        test_support::temp_path("imaps_fetch_many/"), test_support::temp_path("imaps_fetch_many/"));
    auto router2 = std::make_shared<router::StaticShardRouter>(
        std::vector<std::pair<std::string, int>>{},
        0,
        std::vector<std::shared_ptr<DBPool>>{db},
        std::vector<std::shared_ptr<st::IStorageProvider>>{storage});

    ServerConfig cfg2;
    cfg2.perf_mode = true; cfg2.apply_perf_mode();
    cfg2.use_database = false; cfg2.system_domain = "test.local";
    cfg2.storage.local.mail_path = test_support::temp_path("imaps_fetch_many");
    cfg2.storage.local.attachment_path = test_support::temp_path("imaps_fetch_many");
    auto server2 = std::shared_ptr<TestServer>(new TestServer(cfg2, io, wk, router2));
    auto fsm2 = std::make_shared<TraditionalImapsFsm<MockConnection>>(io, wk, router2);

    const int N = 400;   // 旧实现 >~200 就爆栈
    std::vector<std::map<std::string, std::string>> rows;
    rows.reserve(N);
    for (int i = 1; i <= N; ++i) {
        std::string bp = test_support::temp_path("imaps_fetch_many/mail_") + std::to_string(i);
        { std::ofstream f(bp, std::ios::binary); f << "Subject: m" << i << "\r\n\r\nbody" << i; }
        rows.push_back({{"id", std::to_string(1000 + i)}, {"sender", "a@t.local"},
            {"recipient", "b@t.local"}, {"subject", "m"}, {"body_path", bp},
            {"is_starred", "0"}, {"is_deleted", "0"}, {"is_important", "0"},
            {"status", "1"}, {"send_time", "1700000000"}});
    }
    db->mock_conn()->push_sync_result(std::make_shared<test::MockDbResult>(std::move(rows)));

    auto conn_u = std::make_unique<MockConnection>();
    auto* conn_ptr = conn_u.get();
    auto session = std::make_shared<ImapsSession<MockConnection>>(server2.get(), std::move(conn_u), fsm2);
    auto* c = static_cast<ImapContext*>(session->get_context());
    c->is_authenticated = true; c->user_id = 1; c->mailbox_selected = true;
    c->selected_mailbox_id = 1; c->current_tag = "A001";
    session->set_current_state(static_cast<int>(ImapState::SELECTED));

    session->handle_read("A001 FETCH 1:" + std::to_string(N) + " (FLAGS RFC822.SIZE)\r\n");
    session->process_read();

    // 迭代链 + 响应串行化:轮询等完成标记(400 封,预算 5s)
    for (int waited = 0; waited < 5000 && conn_ptr->written().find(
            "A001 OK FETCH completed") == std::string::npos; waited += 5) {
        conn_ptr->pump_executor();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    conn_ptr->pump_executor();
    auto w = conn_ptr->written();
    assert(w.find("A001 OK FETCH completed") != std::string::npos);
    assert(w.find("RFC822.SIZE") != std::string::npos);
    std::filesystem::remove_all(test_support::temp_path("imaps_fetch_many"));
    io->stop(); wk->stop();
    // 收尾：同 fetch_full_path_with_storage —— 手工 session 无 Handle 兜底。
    finish_session(session);
    std::cout << "  [PASS] fetch_many_mails_no_stack_overflow (" << N << " mails, iterative chain)" << std::endl;
}

// literal 声明大小超限 → BAD + 断开（OOM 防线）
TEST(literal_too_large_rejected) {
    auto h = fx.make_session();
    h.session->handle_read("A001 APPEND INBOX (\\Flags) {4294967295+}\r\n");
    for (int waited = 0; waited < 2000 && h.conn->written().find("BAD") == std::string::npos; waited += 5) {
        h.conn->pump_executor();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    h.conn->pump_executor();
    auto w = h.conn->written();
    assert(w.find("BAD") != std::string::npos);
    assert(w.find("Literal too large") != std::string::npos);
    assert(h.session->is_closed());
    std::cout << "  [PASS] literal_too_large_rejected" << std::endl;
}

// ========== 命令顺序错误 ==========

TEST(invalid_command_in_state) {
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::STORE, "H001");
    h.conn->pump_executor();
    auto w = h.conn->written();
    assert(HAS(w, "H001 BAD") || HAS(w, "H001 NO"));
    std::cout << "  [PASS] invalid_command_in_state" << std::endl;
}

// ========== 新增: 无需 DB 的命令 ==========

TEST(logout_without_login) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->current_tag = "A001";
    h.session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::LOGOUT, "A001");
    h.conn->pump_executor();
    auto w = h.conn->written();
    // LOGOUT 总是允许的
    assert(HAS(w, "BYE"));
    assert(HAS(w, "A001 OK"));
    std::cout << "  [PASS] logout_without_login" << std::endl;
}

TEST(check_in_authenticated) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    ctx->current_tag = "C001";
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::CHECK, "C001");
    h.conn->pump_executor();
    auto w = h.conn->written();
    assert(HAS(w, "C001 OK CHECK") || HAS(w, "C001 BAD") || HAS(w, "C001 NO"));
    std::cout << "  [PASS] check_in_authenticated" << std::endl;
}

TEST(noop_in_selected) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    ctx->mailbox_selected = true;
    ctx->current_tag = "B002";
    h.session->set_current_state(static_cast<int>(ImapState::SELECTED));
    fx.fsm->process_event(h.session, ImapEvent::NOOP, "B002");
    h.conn->pump_executor();
    auto w = h.conn->written();
    assert(HAS(w, "B002 OK NOOP"));
    std::cout << "  [PASS] noop_in_selected" << std::endl;
}

TEST(capability_in_authenticated) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::CAPABILITY, "C002");
    h.conn->pump_executor();
    auto w = h.conn->written();
    assert(HAS(w, "CAPABILITY IMAP4rev1"));
    std::cout << "  [PASS] capability_in_authenticated" << std::endl;
}

// ========== 新增: DB 依赖命令（无 DB 时须优雅失败） ==========

TEST(select_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    ctx->current_tag = "S001";
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::SELECT, "S001");
    h.conn->pump_executor();
    auto w = h.conn->written();
    // 无 DB 时返回 NO 或 BAD，不应崩溃
    assert(!w.empty());
    std::cout << "  [PASS] select_no_db" << std::endl;
}

TEST(examine_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::EXAMINE, "E001");
    h.conn->pump_executor();
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] examine_no_db" << std::endl;
}

TEST(create_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::CREATE, "CR001");
    h.conn->pump_executor();
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] create_no_db" << std::endl;
}

TEST(delete_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::DELETE, "D001");
    h.conn->pump_executor();
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] delete_no_db" << std::endl;
}

TEST(rename_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::RENAME, "R001");
    h.conn->pump_executor();
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] rename_no_db" << std::endl;
}

TEST(subscribe_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::SUBSCRIBE, "SUB001");
    h.conn->pump_executor();
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] subscribe_no_db" << std::endl;
}

TEST(unsubscribe_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::UNSUBSCRIBE, "UNS001");
    h.conn->pump_executor();
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] unsubscribe_no_db" << std::endl;
}

TEST(list_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::LIST, "L001");
    h.conn->pump_executor();
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] list_no_db" << std::endl;
}

TEST(lsub_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::LSUB, "LS001");
    h.conn->pump_executor();
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] lsub_no_db" << std::endl;
}

TEST(status_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::IMAP_STATUS, "ST001");
    h.conn->pump_executor();
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] status_no_db" << std::endl;
}

TEST(append_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::APPEND, "AP001");
    h.conn->pump_executor();
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] append_no_db" << std::endl;
}

TEST(search_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    ctx->mailbox_selected = true;
    ctx->current_tag = "SE001";
    h.session->set_current_state(static_cast<int>(ImapState::SELECTED));
    fx.fsm->process_event(h.session, ImapEvent::SEARCH, "SE001");
    h.conn->pump_executor();
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] search_no_db" << std::endl;
}

TEST(fetch_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    ctx->mailbox_selected = true;
    ctx->current_tag = "F001";
    h.session->set_current_state(static_cast<int>(ImapState::SELECTED));
    fx.fsm->process_event(h.session, ImapEvent::FETCH, "F001");
    h.conn->pump_executor();
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] fetch_no_db" << std::endl;
}

TEST(store_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    ctx->mailbox_selected = true;
    ctx->current_tag = "ST001";
    h.session->set_current_state(static_cast<int>(ImapState::SELECTED));
    fx.fsm->process_event(h.session, ImapEvent::STORE, "ST001");
    h.conn->pump_executor();
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] store_no_db" << std::endl;
}

TEST(copy_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    ctx->mailbox_selected = true;
    ctx->current_tag = "CP001";
    h.session->set_current_state(static_cast<int>(ImapState::SELECTED));
    fx.fsm->process_event(h.session, ImapEvent::COPY, "CP001");
    h.conn->pump_executor();
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] copy_no_db" << std::endl;
}

TEST(move_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    ctx->mailbox_selected = true;
    ctx->current_tag = "MV001";
    h.session->set_current_state(static_cast<int>(ImapState::SELECTED));
    fx.fsm->process_event(h.session, ImapEvent::MOVE, "MV001");
    h.conn->pump_executor();
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] move_no_db" << std::endl;
}

TEST(expunge_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    ctx->mailbox_selected = true;
    ctx->current_tag = "EX001";
    h.session->set_current_state(static_cast<int>(ImapState::SELECTED));
    fx.fsm->process_event(h.session, ImapEvent::EXPUNGE, "EX001");
    h.conn->pump_executor();
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] expunge_no_db" << std::endl;
}

TEST(close_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    ctx->mailbox_selected = true;
    ctx->current_tag = "CL001";
    h.session->set_current_state(static_cast<int>(ImapState::SELECTED));
    fx.fsm->process_event(h.session, ImapEvent::CLOSE, "CL001");
    h.conn->pump_executor();
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] close_no_db" << std::endl;
}

TEST(idle_initiated) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::IDLE, "I001");
    h.conn->pump_executor();
    auto w = h.conn->written();
    // IDLE 初始化应返回 continuation 或 tagged 响应
    assert(!w.empty());
    std::cout << "  [PASS] idle_initiated" << std::endl;
}

// ========== CONNECT (INIT → greeting) ==========

TEST(connect_greeting) {
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::INIT));
    fx.fsm->process_event(h.session, ImapEvent::CONNECT, "");
    h.conn->pump_executor();
    auto w = h.conn->written();
    assert(HAS(w, "* OK"));
    std::cout << "  [PASS] connect_greeting" << std::endl;
}

// ========== AUTHENTICATE (SASL) ==========

TEST(authenticate_not_supported) {
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::AUTHENTICATE, "C001");
    h.conn->pump_executor();
    auto w = h.conn->written();
    assert(HAS(w, "NO") || HAS(w, "BAD"));
    std::cout << "  [PASS] authenticate_unsupported" << std::endl;
}

// ========== ERROR / TIMEOUT ==========

TEST(error_event) {
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::ERROR, "");
    h.conn->pump_executor();
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] error_event" << std::endl;
}

TEST(timeout_event) {
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::TIMEOUT, "");
    h.conn->pump_executor();
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] timeout_event" << std::endl;
}

// ========== IDLE + DONE (deferred read) ==========

TEST(idle_with_done) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    // 防止 do_async_read→eof→close 杀死连接
    h.conn->set_deferred_read(true);

    fx.fsm->process_event(h.session, ImapEvent::IDLE, "I001");
    h.conn->pump_executor();
    auto w = h.conn->written();
    assert(HAS(w, "+ idling"));

    // DONE 命令到达 (模拟客户端在 IDLE 中发送)
    h.conn->clear_written();
    fx.fsm->process_event(h.session, ImapEvent::DONE, "I001");
    h.conn->pump_executor();
    // handle_done（DONE handler，2026-10-22 注册）写入 "I001 OK IDLE terminated"
    // 并重新 arm 看门狗。此前本用例断言的是 clear_written 之前捕获的旧缓冲
    //（实际空断言），顺手修正为断言真实 DONE 响应。
    auto w2 = h.conn->written();
    assert(HAS(w2, "I001 OK IDLE terminated"));
    // DONE 后 IDLE 推送注册已注销
    assert(IdlePushRegistry::instance().watcher_count("user0@test.local") == 0);
    // 测试结束：关闭连接，释放 deferred read handler（否则其捕获 shared_from_this
    // 的 handler 与 session 形成引用环，session 永不析构 → LSan 泄漏）
    h.session->close();
    std::cout << "  [PASS] idle_with_done (response: " << w2.substr(0, 60) << ")" << std::endl;
}

// ========== IDLE push（本地投递 → untagged EXISTS，2026-10-22） ==========
// 仿 mailbox_repository_injection：假 mailbox 仓储控制 EXISTS 计数，模拟
// SMTP 投递提交后 mail_mailbox 行落库对计数的影响。注册表直接
// notify_account(account)（与 smtps DATA_END 持久化成功钩子同一入口）。
class IdlePushMailboxRepository final : public repository::IMailboxRepository {
public:
    std::atomic<size_t> exists{3};
    void get_mailboxes_async(Lease, uint64_t, ListCallback cb) override { cb(true, {}); }
    void find_mailbox_id_async(Lease, uint64_t, const std::string&, std::function<void(uint64_t)> cb) override { cb(77); }
    void get_mailbox_mails_async(Lease, uint64_t, uint64_t, MessagesCallback cb) override { cb(true, {}); }
    void get_mailbox_count_async(Lease, uint64_t, uint64_t, std::function<void(size_t)> cb) override { cb(exists.load()); }
    void get_mailbox_unseen_count_async(Lease, uint64_t, uint64_t, std::function<void(size_t)> cb) override { cb(0); }
    void get_mailbox_uidnext_async(Lease, uint64_t, uint64_t, std::function<void(uint64_t)> cb) override { cb(1); }
};

// 独立 router/FSM：带 MockDbPool（真实 SELECT 有租约有效性预检），仓储注入假读取。
struct IdlePushFixture {
    std::shared_ptr<test::MockDbPool> pool = std::make_shared<test::MockDbPool>();
    std::shared_ptr<router::StaticShardRouter> shards;
    std::shared_ptr<TestServer> server;
    std::shared_ptr<TraditionalImapsFsm<MockConnection>> fsm;
    std::shared_ptr<IdlePushMailboxRepository> reads = std::make_shared<IdlePushMailboxRepository>();

    explicit IdlePushFixture(FsmTestFixture& fx) {
        shards = std::make_shared<router::StaticShardRouter>(
            std::vector<std::pair<std::string, int>>{}, 0,
            std::vector<std::shared_ptr<DBPool>>{pool},
            std::vector<std::shared_ptr<storage::IStorageProvider>>{});
        server = std::make_shared<TestServer>(fx.cfg, fx.io_pool, fx.worker_pool, shards);
        fsm = std::make_shared<TraditionalImapsFsm<MockConnection>>(fx.io_pool, fx.worker_pool, shards);
        fsm->set_mailbox_repository(reads);
    }

    // 已认证 + 已选中 INBOX（user_id 41 / mailbox_id 77），SELECT 基线取当前计数
    FsmTestFixture::Handle make_session() {
        auto connection = std::make_unique<MockConnection>();
        auto* raw = connection.get();
        raw->set_deferred_read(true);   // IDLE 长等待：防 read→EOF→close
        auto session = std::make_shared<ImapsSession<MockConnection>>(server.get(), std::move(connection), fsm);
        auto* ctx = static_cast<ImapContext*>(session->get_context());
        ctx->is_authenticated = true;
        ctx->username = "user0@test.local";
        ctx->user_id = 41;
        ctx->shard_index = 0;
        ctx->mailbox_selected = true;
        ctx->selected_mailbox_name = "INBOX";
        ctx->selected_mailbox_id = 77;
        ctx->announced_exists = reads->exists.load();   // SELECT 完成时写入的基线
        session->set_current_state(static_cast<int>(ImapState::SELECTED));
        return {raw, session};
    }
};

TEST(idle_push_exists_on_delivery) {
    IdlePushRegistry::instance().reset();
    {
        IdlePushFixture ifx(fx);
        auto h = ifx.make_session();
        ifx.fsm->process_event(h.session, ImapEvent::IDLE, "I001");
        h.conn->pump_executor();
        assert(HAS(h.conn->written(), "+ idling"));
        // 注册表键大小写不敏感（SMTP 侧 RCPT 地址大小写随意）
        assert(IdlePushRegistry::instance().watcher_count("user0@test.local") == 1);
        assert(IdlePushRegistry::instance().watcher_count("USER0@TEST.LOCAL") == 1);

        // 投递事件：3 → 5，会话应收到 untagged EXISTS
        ifx.reads->exists.store(5);
        IdlePushRegistry::instance().notify_account("user0@test.local");
        h.conn->pump_executor();   // 排空：post 会话 executor → 计数查询 → post 回 → 写
        auto w = h.conn->written();
        assert(HAS(w, "* 5 EXISTS"));
        auto* ctx = static_cast<ImapContext*>(h.session->get_context());
        assert(ctx->announced_exists == 5);

        // 计数不变 → 不重复宣告（grow-only）
        h.conn->clear_written();
        IdlePushRegistry::instance().notify_account("user0@test.local");
        h.conn->pump_executor();
        assert(h.conn->written().find("EXISTS") == std::string::npos);

        // 缩小（另一会话 EXPUNGE，MVP 不推 EXPUNGE）→ 不推、基线不回退；
        // 之后涨过基线仍能正确补发
        ifx.reads->exists.store(4);
        IdlePushRegistry::instance().notify_account("user0@test.local");
        h.conn->pump_executor();
        assert(h.conn->written().find("EXISTS") == std::string::npos);
        assert(ctx->announced_exists == 5);
        ifx.reads->exists.store(6);
        IdlePushRegistry::instance().notify_account("user0@test.local");
        h.conn->pump_executor();
        assert(HAS(h.conn->written(), "* 6 EXISTS"));
    }
    IdlePushRegistry::instance().reset();
    std::cout << "  [PASS] idle_push_exists_on_delivery" << std::endl;
}

TEST(idle_push_ignores_session_after_done) {
    IdlePushRegistry::instance().reset();
    {
        IdlePushFixture ifx(fx);
        auto h = ifx.make_session();
        ifx.fsm->process_event(h.session, ImapEvent::IDLE, "I001");
        h.conn->pump_executor();
        assert(HAS(h.conn->written(), "+ idling"));

        // 走真实 wire 路径退出 IDLE：客户端发裸 "DONE"（无 tag），经
        // process_read 收口到 FSM handle_done（注销注册 + 重新 arm 看门狗 + OK）
        h.conn->trigger_deferred_read("DONE\r\n");
        h.conn->pump_executor();
        assert(HAS(h.conn->written(), "OK IDLE terminated"));
        assert(IdlePushRegistry::instance().watcher_count("user0@test.local") == 0);

        // DONE 后投递事件 → 什么都不写
        h.conn->clear_written();
        ifx.reads->exists.store(9);
        IdlePushRegistry::instance().notify_account("user0@test.local");
        h.conn->pump_executor();
        assert(h.conn->written().find("EXISTS") == std::string::npos);

        // 看门狗 re-arm 路径不破坏会话：退出 IDLE 后 NOOP 正常应答
        ifx.fsm->process_event(h.session, ImapEvent::NOOP, "N001");
        h.conn->pump_executor();
        assert(HAS(h.conn->written(), "NOOP completed"));
    }
    IdlePushRegistry::instance().reset();
    std::cout << "  [PASS] idle_push_ignores_session_after_done" << std::endl;
}

TEST(idle_push_cleans_up_destroyed_session) {
    IdlePushRegistry::instance().reset();
    {
        IdlePushFixture ifx(fx);
        {
            auto h = ifx.make_session();
            ifx.fsm->process_event(h.session, ImapEvent::IDLE, "I001");
            h.conn->pump_executor();
            assert(IdlePushRegistry::instance().watcher_count("user0@test.local") == 1);
        }
        // Handle 析构（finish_session: close + 排空）→ 会话已销毁。注册表无
        // 显式注销调用（IDLE 中被断开的会话没有 DONE），只能靠 weak 过期兜底。
        assert(IdlePushRegistry::instance().watcher_count("user0@test.local") == 1);
        // 销毁后的投递事件：不崩溃，且过期条目被顺手清理
        ifx.reads->exists.store(8);
        IdlePushRegistry::instance().notify_account("user0@test.local");
        assert(IdlePushRegistry::instance().watcher_count("user0@test.local") == 0);
    }
    IdlePushRegistry::instance().reset();
    std::cout << "  [PASS] idle_push_cleans_up_destroyed_session" << std::endl;
}

TEST(idle_push_select_sets_exists_baseline) {
    IdlePushRegistry::instance().reset();
    {
        IdlePushFixture ifx(fx);
        ifx.reads->exists.store(4);
        auto connection = std::make_unique<MockConnection>();
        auto* raw = connection.get();
        raw->set_deferred_read(true);
        auto session = std::make_shared<ImapsSession<MockConnection>>(ifx.server.get(), std::move(connection), ifx.fsm);
        auto* ctx = static_cast<ImapContext*>(session->get_context());
        ctx->is_authenticated = true;
        ctx->username = "user0@test.local";
        ctx->user_id = 41;
        ctx->shard_index = 0;
        session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
        FsmTestFixture::Handle h{raw, session};

        // 真实 SELECT：响应里的 EXISTS 数同时写进 ctx 基线
        ifx.fsm->process_event(h.session, ImapEvent::SELECT, "S001");
        h.conn->pump_executor();
        auto w = h.conn->written();
        assert(HAS(w, "* 4 EXISTS"));
        assert(ctx->announced_exists == 4);

        // IDLE 中投递两封 → 只补发增量 6，不重发 4
        ifx.fsm->process_event(h.session, ImapEvent::IDLE, "I001");
        h.conn->pump_executor();
        assert(HAS(h.conn->written(), "+ idling"));
        ifx.reads->exists.store(6);
        IdlePushRegistry::instance().notify_account("user0@test.local");
        h.conn->pump_executor();
        w = h.conn->written();
        assert(HAS(w, "* 6 EXISTS"));
        assert(w.find("* 4 EXISTS") == w.rfind("* 4 EXISTS"));   // 旧计数只出现一次（SELECT 那次）
    }
    IdlePushRegistry::instance().reset();
    std::cout << "  [PASS] idle_push_select_sets_exists_baseline" << std::endl;
}

// ========== UIDNEXT 高水位（原子推进，防并发撞值/expunge 回退） ==========
// An application repository can replace SQL-backed reads without changing the
// protocol FSM, including modified UTF-7 decoding before the repository call.
class FakeMailboxRepository final : public repository::IMailboxRepository {
public:
    std::string found_name;
    void get_mailboxes_async(Lease, uint64_t, ListCallback cb) override { cb(true, {{77, "injected", 1}}); }
    void find_mailbox_id_async(Lease, uint64_t, const std::string& name, std::function<void(uint64_t)> cb) override {
        found_name = name; cb(77);
    }
    void get_mailbox_mails_async(Lease, uint64_t, uint64_t, MessagesCallback cb) override { cb(true, {}); }
    void get_mailbox_count_async(Lease, uint64_t, uint64_t, std::function<void(size_t)> cb) override { cb(4); }
    void get_mailbox_unseen_count_async(Lease, uint64_t, uint64_t, std::function<void(size_t)> cb) override { cb(2); }
    void get_mailbox_uidnext_async(Lease, uint64_t, uint64_t, std::function<void(uint64_t)> cb) override { cb(88); }
};
TEST(mailbox_repository_injection) {
    auto injected = std::make_shared<FakeMailboxRepository>();
    auto original = fx.fsm->get_mailbox_repository();
    fx.fsm->set_mailbox_repository(injected);
    int calls = 0;
    fx.fsm->get_mailboxes_async({}, 41, [&](bool ok, std::vector<MailboxSummary> boxes) {
        assert(ok && boxes.size() == 1 && boxes[0].name == "injected"); ++calls;
    });
    fx.fsm->find_mailbox_id_async({}, 41, imap_utils::encode_mailbox_name("收件箱"), [&](uint64_t id) {
        assert(id == 77); ++calls;
    });
    assert(injected->found_name == "收件箱");
    fx.fsm->get_mailbox_uidnext_async({}, 77, 41, [&](uint64_t value) { assert(value == 88); ++calls; });
    assert(calls == 3);
    fx.fsm->set_mailbox_repository(original);
    std::cout << "  [PASS] mailbox repository injection and protocol name decoding" << std::endl;
}

// Write repositories can be replaced independently of mailbox reads. Their
// delayed completion must return to the session executor before touching caches
// or producing protocol responses.
class MutationMailboxReads final : public repository::IMailboxRepository {
public:
    uint64_t target = 77;
    bool fail_messages = false;
    void get_mailboxes_async(Lease, uint64_t, ListCallback cb) override { cb(true, {}); }
    void find_mailbox_id_async(Lease, uint64_t, const std::string&, std::function<void(uint64_t)> cb) override { cb(target); }
    void get_mailbox_mails_async(Lease, uint64_t, uint64_t, MessagesCallback cb) override {
        if (fail_messages) { cb(false, {}); return; }
        MailboxMailInfo first; first.mail_id = 501; first.status = 1;
        MailboxMailInfo second; second.mail_id = 502; second.is_deleted = true;
        cb(true, {first, second});
    }
    void get_mailbox_count_async(Lease, uint64_t, uint64_t, std::function<void(size_t)> cb) override { cb(2); }
    void get_mailbox_unseen_count_async(Lease, uint64_t, uint64_t, std::function<void(size_t)> cb) override { cb(1); }
    void get_mailbox_uidnext_async(Lease, uint64_t, uint64_t, std::function<void(uint64_t)> cb) override { cb(503); }
};
class DelayedMutationRepository final : public repository::IImapMutationRepository {
public:
    repository::ImapStoreRequest store_request;
    repository::ImapCopyMoveRequest copy_request;
    repository::ImapMailboxScope expunge_scope;
    Complete store_complete, expunge_complete;
    CopyComplete copy_complete;
    int stores = 0, copies = 0, expunges = 0;
    void store_async(Lease lease, repository::ImapStoreRequest request, Complete complete) override {
        ++stores; store_request = std::move(request);
        store_complete = [lease = std::move(lease), complete = std::move(complete)](repository::ImapMutationResult result) {
            assert(lease && lease->is_valid()); complete(result);
        };
    }
    void expunge_async(Lease lease, repository::ImapMailboxScope scope, Complete complete) override {
        ++expunges; expunge_scope = scope;
        expunge_complete = [lease = std::move(lease), complete = std::move(complete)](repository::ImapMutationResult result) {
            assert(lease && lease->is_valid()); complete(result);
        };
    }
    void copy_move_async(Lease lease, repository::ImapCopyMoveRequest request, CopyComplete complete) override {
        ++copies; copy_request = std::move(request);
        copy_complete = [lease = std::move(lease), complete = std::move(complete)](repository::ImapCopyMoveResult result) {
            assert(lease && lease->is_valid()); complete(result);
        };
    }
    void finish_store(repository::ImapMutationResult result) { auto cb = std::move(store_complete); assert(cb); cb(result); }
    void finish_expunge(repository::ImapMutationResult result) { auto cb = std::move(expunge_complete); assert(cb); cb(result); }
    void finish_copy(repository::ImapCopyMoveResult result) { auto cb = std::move(copy_complete); assert(cb); cb(result); }
};
struct MutationProtocolFixture {
    using Fsm = TraditionalImapsFsm<MockConnection>;
    std::shared_ptr<test::MockDbPool> pool = std::make_shared<test::MockDbPool>();
    std::shared_ptr<DelayedMutationRepository> mutations = std::make_shared<DelayedMutationRepository>();
    std::shared_ptr<MutationMailboxReads> reads = std::make_shared<MutationMailboxReads>();
    std::shared_ptr<Fsm> fsm;
    std::shared_ptr<TestServer> server;
    std::shared_ptr<Fsm::MailboxListCache> lists = std::make_shared<Fsm::MailboxListCache>(8, std::chrono::seconds(60));
    std::shared_ptr<Fsm::MailboxStatsCache> stats = std::make_shared<Fsm::MailboxStatsCache>(8, std::chrono::seconds(60));
    explicit MutationProtocolFixture(FsmTestFixture& fx, std::shared_ptr<storage::IStorageProvider> provider = nullptr,
                                     std::shared_ptr<pr::ThreadPoolBase> worker = nullptr) {
        if (!worker) worker = fx.worker_pool;
        auto shards = std::make_shared<router::StaticShardRouter>(
            std::vector<std::pair<std::string, int>>{}, 0,
            std::vector<std::shared_ptr<DBPool>>{pool},
            std::vector<std::shared_ptr<storage::IStorageProvider>>{std::move(provider)});
        server = std::make_shared<TestServer>(fx.cfg, fx.io_pool, worker, shards);
        fsm = std::make_shared<Fsm>(fx.io_pool, worker, shards);
        fsm->set_mailbox_repository(reads);
        fsm->set_mutation_repository(mutations);
        fsm->set_mailbox_list_cache(lists);
        fsm->set_mailbox_stats_cache(stats);
        for (uint64_t mailbox : {7, 77, 88}) {
            lists->put(mbox_cache_key(41, mailbox), {});
            stats->put(mbox_cache_key(41, mailbox), {});
        }
    }
    FsmTestFixture::Handle make_session() {
        auto connection = std::make_unique<MockConnection>();
        auto* raw = connection.get();
        raw->set_deferred_read(true);
        auto session = std::make_shared<ImapsSession<MockConnection>>(server.get(), std::move(connection), fsm);
        auto* ctx = static_cast<ImapContext*>(session->get_context());
        ctx->is_authenticated = true; ctx->mailbox_selected = true;
        ctx->user_id = 41; ctx->shard_index = 0; ctx->selected_mailbox_id = 7;
        ctx->selected_mailbox_name = "INBOX"; ctx->username = "owner@test.local";
        session->set_current_state(static_cast<int>(ImapState::SELECTED));
        return {raw, session};
    }
    bool has_list(uint64_t mailbox) {
        MailboxListEntry entry; bool stale = false;
        return lists->get(mbox_cache_key(41, mailbox), entry, stale);
    }
    bool has_stats(uint64_t mailbox) {
        MailboxCacheEntry entry; bool stale = false;
        return stats->get(mbox_cache_key(41, mailbox), entry, stale);
    }
};
TEST(search_missing_layout_returns_no_and_resumes) {
    struct ManualWorkers final : pr::ThreadPoolBase {
        std::vector<std::function<void()>> tasks;
        void start() override {}
        void stop(bool = true) override {}
        size_t thread_count() const override { return 1; }
        bool is_running() const override { return true; }
        void post_impl(std::function<void()> task) override { tasks.push_back(std::move(task)); }
    };
    auto generic = std::make_shared<test::NoMailLayoutStorage>();
    auto workers = std::make_shared<ManualWorkers>();
    MutationProtocolFixture fixture(fx, generic, workers);
    fixture.fsm->invalidate_mailbox_list(41, 7);
    auto h = fixture.make_session();
    // handle_read accepts one extracted command, not a whole TCP read. Feed the
    // connection so NOOP remains in the actual session buffer while SEARCH pauses.
    h.conn->set_read_data("K1 SEARCH HEADER Subject probe\r\nK2 NOOP\r\n");
    h.session->do_async_read(); h.conn->pump_executor();
    assert(workers->tasks.size() == 1 && h.session->is_paused() && h.session->has_buffered_input());
    assert(h.conn->written().empty());
    auto search = std::move(workers->tasks.front()); workers->tasks.clear();
    search(); // A Worker completion outside the connection owner must only post.
    assert(h.conn->written().empty() && h.session->is_paused() && h.session->has_buffered_input());
    h.conn->pump_executor();
    const auto written = h.conn->written();
    const auto failed_search = written.find("K1 NO Cannot read message object\r\n");
    const auto resumed_noop = written.find("K2 OK NOOP completed\r\n");
    assert(failed_search != std::string::npos && resumed_noop != std::string::npos && failed_search < resumed_noop);
    assert(!HAS(written, "* SEARCH") && !h.session->is_paused() && !h.session->has_buffered_input() && !generic->io_calls);
    assert(!h.session->is_closed() && h.conn->has_pending_read());
    std::cout << "  [PASS] SEARCH missing layout completes on owner and resumes buffered commands" << std::endl;
}
TEST(mutation_repository_store_and_expunge_injection) {
    {
        MutationProtocolFixture fixture(fx);
        auto h = fixture.make_session();
        bool rejected = false;
        try { fixture.fsm->set_mutation_repository({}); } catch (const std::invalid_argument&) { rejected = true; }
        assert(rejected && fixture.fsm->get_mutation_repository() == fixture.mutations);
        h.session->handle_read("W1 STORE 1 +FLAGS (\\Seen \\Flagged \\Deleted)\r\n");
        h.session->process_read();
        auto& request = fixture.mutations->store_request;
        assert(fixture.mutations->stores == 1 && request.scope.user_id == 41 && request.scope.mailbox_id == 7);
        assert((request.mail_ids == std::vector<uint64_t>{501}));
        assert(request.seen.value() && request.flagged.value() && request.deleted.value());
        assert(request.recipient_fallback == "owner@test.local");
        assert(!fixture.pool->mock_conn()->has_pending_query() && !fixture.pool->mock_conn()->has_pending_execute());
        fixture.mutations->finish_store({true});
        assert(fixture.has_list(7) && fixture.has_stats(7) && h.conn->written().empty());
        h.conn->pump_executor();
        assert(!fixture.has_list(7) && !fixture.has_stats(7) && fixture.has_list(88));
        assert(HAS(h.conn->written(), "* 1 FETCH (FLAGS (") && HAS(h.conn->written(), "W1 OK STORE completed"));
    }
    {
        MutationProtocolFixture fixture(fx);
        auto h = fixture.make_session();
        h.session->handle_read("W2 EXPUNGE\r\n");
        h.session->process_read();
        assert(fixture.mutations->expunges == 1 && fixture.mutations->expunge_scope.user_id == 41
               && fixture.mutations->expunge_scope.mailbox_id == 7);
        fixture.mutations->finish_expunge({true});
        assert(fixture.has_list(7) && h.conn->written().empty());
        h.conn->pump_executor();
        assert(!fixture.has_list(7) && fixture.has_list(88));
        assert(HAS(h.conn->written(), "* 2 EXPUNGE\r\n") && HAS(h.conn->written(), "W2 OK EXPUNGE completed"));
    }
    std::cout << "  [PASS] injected STORE/EXPUNGE resume on the session executor and invalidate caches" << std::endl;
}
TEST(mutation_repository_copy_move_failures) {
    struct Case { bool move; repository::ImapCopyMoveResult result; bool ok; };
    const Case cases[] = {
        {false, {false, 0, false}, false}, {true, {false, 0, false}, false},
        {true, {true, 2, false}, false}, {true, {true, 0, true}, true},
        {false, {true, 0, false}, true}
    };
    for (const auto& test : cases) {
        MutationProtocolFixture fixture(fx);
        auto h = fixture.make_session();
        const std::string command = test.move ? "MOVE" : "COPY";
        h.session->handle_read("W3 " + command + " 1 Archive\r\n");
        h.session->process_read();
        const auto& request = fixture.mutations->copy_request;
        assert(fixture.mutations->copies == 1 && request.move == test.move && request.target_mailbox_id == 77);
        assert(request.source.user_id == 41 && request.source.mailbox_id == 7);
        assert((request.mail_ids == std::vector<uint64_t>{501}));
        fixture.mutations->finish_copy(test.result);
        assert(fixture.has_list(7) && fixture.has_list(77) && h.conn->written().empty());
        h.conn->pump_executor();
        assert(!fixture.has_list(77) && fixture.has_list(88));
        assert(fixture.has_list(7) == !test.move);
        if (test.ok) assert(HAS(h.conn->written(), "W3 OK " + command + " completed (0 messages)"));
        else assert(HAS(h.conn->written(), "W3 NO " + command + " failed: database error"));
    }
    std::cout << "  [PASS] COPY/MOVE database failures return NO, successful zero copies remain valid" << std::endl;
}
TEST(mutation_repository_same_mailbox_move_is_rejected) {
    for (bool move : {true, false}) {
        MutationProtocolFixture fixture(fx);
        fixture.reads->target = 7;
        fixture.fsm->set_mutation_repository(repository::make_sql_imap_mutation_repository());
        auto h = fixture.make_session();
        const std::string command = move ? "MOVE" : "COPY";
        h.session->handle_read("W5 " + command + " 1 INBOX\r\n");
        h.session->process_read();
        auto db = fixture.pool->mock_conn();
        if (move) {
            assert(db->last_query_sql().empty() && !db->has_pending_execute() && !db->has_pending_query());
        } else {
            assert(db->last_query_sql().find("INSERT IGNORE") == 0 && db->has_pending_execute());
            db->fire_execute(true);
            db->fire_query(std::make_shared<test::MockDbResult>(
                std::vector<std::map<std::string, std::string>>{{{"affected", "0"}}}));
            assert(!db->has_pending_execute());
        }
        h.conn->pump_executor();
        if (move) assert(HAS(h.conn->written(), "W5 NO MOVE failed: database error"));
        else assert(HAS(h.conn->written(), "W5 OK COPY completed (0 messages)"));
    }
    std::cout << "  [PASS] same-mailbox MOVE cannot delete its only link; COPY remains harmless" << std::endl;
}
TEST(mutation_repository_close_resumes_context_on_executor) {
    MutationProtocolFixture fixture(fx);
    auto h = fixture.make_session();
    h.session->handle_read("W4 CLOSE\r\n");
    h.session->process_read();
    assert(fixture.mutations->expunges == 1);
    fixture.mutations->finish_expunge({true});
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    assert(ctx->mailbox_selected && fixture.has_list(7));
    h.conn->pump_executor();
    assert(!ctx->mailbox_selected && !fixture.has_list(7));
    assert(HAS(h.conn->written(), "W4 OK CLOSE completed"));
    std::cout << "  [PASS] CLOSE uses the injected expunge repository and serial context completion" << std::endl;
}

TEST(mutation_failures_return_no_without_false_notifications) {
    for (const std::string command : {"STORE 1 +FLAGS (\\Seen)", "EXPUNGE", "CLOSE"}) {
        MutationProtocolFixture fixture(fx);
        auto h = fixture.make_session();
        h.session->handle_read("F1 " + command + "\r\n");
        h.session->process_read();
        if (command.find("STORE") == 0) fixture.mutations->finish_store({false});
        else fixture.mutations->finish_expunge({false});
        assert(fixture.has_list(7) && fixture.has_stats(7));
        h.conn->pump_executor();
        const auto response = h.conn->written();
        assert(HAS(response, "F1 NO") && !HAS(response, "F1 OK"));
        assert(!HAS(response, "* 1 FETCH") && !HAS(response, "* 2 EXPUNGE"));
        assert(!fixture.has_list(7) && !fixture.has_stats(7));
        auto* ctx = static_cast<ImapContext*>(h.session->get_context());
        assert(ctx->mailbox_selected && h.session->get_current_state() == static_cast<int>(ImapState::SELECTED));
        assert(!h.session->is_paused() && h.conn->has_pending_read());
        h.conn->executor()->post([connection = h.conn] { connection->trigger_deferred_read("F2 NOOP\r\n"); });
        h.conn->pump_executor();
        assert(HAS(h.conn->written(), "F2 OK NOOP completed"));
    }
    {
        MutationProtocolFixture fixture(fx);
        fixture.reads->fail_messages = true;
        auto h = fixture.make_session();
        h.session->handle_read("F3 EXPUNGE\r\n");
        h.session->process_read();
        assert(h.conn->written().empty() && !fixture.mutations->expunges);
        h.conn->pump_executor();
        assert(HAS(h.conn->written(), "F3 NO Server error") && !HAS(h.conn->written(), "* 2 EXPUNGE"));
        assert(fixture.has_list(7) && fixture.has_stats(7));
        assert(!h.session->is_paused() && h.conn->has_pending_read());
    }
    std::cout << "  [PASS] STORE/EXPUNGE/CLOSE fail closed without false changes and permit following commands" << std::endl;
}
class InjectedImapMessages final : public repository::IImapMessageRepository {
public:
    void get_mail_async(Lease, uint64_t id, MailComplete cb) override {
        MailboxMailInfo info; info.mail_id = id; info.subject = "injected"; cb(true, info);
    }
    void get_sender_async(Lease, uint64_t, AddressComplete cb) override { cb("sender@test"); }
    void get_recipients_async(Lease, uint64_t, RecipientsComplete cb) override { cb({"recipient@test"}); }
    void get_user_address_async(Lease, uint64_t, AddressComplete cb) override { cb("owner@test"); }
    void insert_mail_async(Lease, repository::ImapMailRecord, Complete cb) override { cb(true); }
    void link_mail_async(Lease, repository::ImapMailLink, Complete cb) override { cb(true); }
    void remove_mail_async(Lease, uint64_t, Complete cb) override { cb(true); }
};
class InjectedImapFolders final : public repository::IImapFolderRepository {
public:
    repository::ImapMailboxScope scope;
    std::string name, operation;
    Complete pending;
    void save(Lease lease, Complete cb) {
        pending = [lease = std::move(lease), cb = std::move(cb)](repository::ImapFolderResult value) {
            assert(lease && lease->is_valid()); cb(value);
        };
    }
    void create_async(Lease lease, uint64_t user, std::string decoded, Complete cb) override {
        operation = "CREATE"; scope = {user, 0}; name = std::move(decoded); save(std::move(lease), std::move(cb));
    }
    void remove_async(Lease lease, repository::ImapMailboxScope value, Complete cb) override {
        operation = "DELETE"; scope = value; save(std::move(lease), std::move(cb));
    }
    void rename_async(Lease lease, repository::ImapMailboxScope value, std::string decoded, Complete cb) override {
        operation = "RENAME"; scope = value; name = std::move(decoded); save(std::move(lease), std::move(cb));
    }
    void finish(repository::ImapFolderResult value) { auto cb = std::move(pending); assert(cb); cb(value); }
};
class InjectedAppendService final : public IImapAppendService {
public:
    ImapAppendRequest request;
    Complete pending;
    void append_async(Lease lease, std::shared_ptr<pr::storage::IStorageProvider>, ImapAppendRequest value, Complete cb) override {
        request = std::move(value);
        pending = [lease = std::move(lease), cb = std::move(cb)](ImapAppendResult result) {
            assert(lease && lease->is_valid()); cb(std::move(result));
        };
    }
    void finish(ImapAppendResult result) { auto cb = std::move(pending); assert(cb); cb(std::move(result)); }
};
TEST(message_repository_read_facades_are_injectable) {
    MutationProtocolFixture fixture(fx);
    fixture.fsm->set_message_repository(std::make_shared<InjectedImapMessages>());
    int calls = 0;
    fixture.fsm->get_mail_info_async({}, 81, [&](bool ok, MailboxMailInfo info) { assert(ok && info.mail_id == 81 && info.subject == "injected"); ++calls; });
    fixture.fsm->get_mail_sender_async({}, 81, [&](std::string value) { assert(value == "sender@test"); ++calls; });
    fixture.fsm->get_mail_recipients_async({}, 81, [&](std::vector<std::string> value) { assert(value.size() == 1 && value[0] == "recipient@test"); ++calls; });
    fixture.fsm->get_user_email_async({}, 41, [&](std::string value) { assert(value == "owner@test"); ++calls; });
    assert(calls == 4 && !fixture.pool->mock_conn()->has_pending_query());
    std::cout << "  [PASS] message metadata read facades delegate without SQL" << std::endl;
}
TEST(folder_repository_protocol_injection) {
    using Status = repository::ImapFolderStatus;
    struct Case { std::string command, name; Status status; bool changed; };
    const std::vector<Case> cases{
        {"CREATE " + imap_utils::encode_mailbox_name("项目"), "项目", Status::ok, true},
        {"DELETE Archive", "", Status::protected_mailbox, false},
        {"DELETE Archive", "", Status::database_error, true},
        {"RENAME Archive " + imap_utils::encode_mailbox_name("新"), "新", Status::database_error, true}
    };
    for (const auto& test : cases) {
        MutationProtocolFixture fixture(fx);
        auto folders = std::make_shared<InjectedImapFolders>();
        fixture.fsm->set_folder_repository(folders);
        auto h = fixture.make_session();
        h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
        h.session->handle_read("D1 " + test.command + "\r\n"); h.session->process_read();
        assert(folders->pending && folders->scope.user_id == 41 && folders->name == test.name);
        if (folders->operation != "CREATE") assert(folders->scope.mailbox_id == 77);
        folders->finish({test.status, test.changed});
        assert(h.conn->written().empty() && fixture.has_list(77));
        h.conn->pump_executor();
        assert(HAS(h.conn->written(), test.status == Status::ok ? "D1 OK" : "D1 NO"));
        if (folders->operation != "CREATE" && test.changed) assert(!fixture.has_list(77) && !fixture.has_stats(77));
        if (!test.changed) assert(fixture.has_list(77));
    }
    std::cout << "  [PASS] decoded folder requests and typed failures remain in the protocol adapter" << std::endl;
}
TEST(append_service_protocol_injection) {
    for (bool ok : {true, false}) {
        MutationProtocolFixture fixture(fx);
        auto append = std::make_shared<InjectedAppendService>(); fixture.fsm->set_append_service(append);
        auto h = fixture.make_session();
        const std::string body = "Subject: injected\r\n\r\nliteral";
        h.session->handle_read("P1 APPEND Archive (\\Draft) {" + std::to_string(body.size()) + "}\r\n");
        h.session->process_read(); h.conn->pump_executor(); h.conn->clear_written();
        h.session->handle_read(body); h.session->process_read();
        assert(append->pending && append->request.user_id == 41 && append->request.mailbox_id == 77);
        assert(append->request.body == body && append->request.initial_status == 1);
        assert(append->request.authenticated_address == "owner@test.local");
        assert(append->request.subject.find("injected") == 0);
        ImapAppendResult result; result.ok = ok; result.mail_id = ok ? 901 : 0;
        if (!ok) result.error = "Insert mail record failed";
        append->finish(std::move(result));
        assert(h.conn->written().empty() && fixture.has_list(77));
        h.conn->pump_executor();
        assert(!fixture.has_list(77) && !fixture.has_stats(77));
        if (ok) assert(HAS(h.conn->written(), "P1 OK [APPENDUID 77 901] APPEND completed"));
        else assert(HAS(h.conn->written(), "P1 NO [SERVERBUG] APPEND failed: Insert mail record failed"));
    }
    std::cout << "  [PASS] APPEND parser delegates a stable business request and resumes on its executor" << std::endl;
}

TEST(uidnext_advance_read) {
    // 剧情：SELECT/STATUS 报告 UIDNEXT。新实现 = 原子推进高水位(advance,写)
    // + 读(advance 后的 mailbox_uidnext 行)。mock 同步模式下 advance 的
    // async_execute 不消耗结果，read 的 async_query 弹出一个 uidnext 值。
    auto io = std::make_shared<pr::IOThreadPool>(1); io->start();
    auto wk = std::make_shared<pr::WorkerThreadPool>(2); wk->start();
    auto db = std::make_shared<test::MockDbPool>();
    db->mock_conn()->set_deferred(false);   // 同步模式
    auto router = std::make_shared<router::StaticShardRouter>(
        std::vector<std::pair<std::string, int>>{},
        0,
        std::vector<std::shared_ptr<DBPool>>{db},
        std::vector<std::shared_ptr<storage::IStorageProvider>>{});
    ServerConfig cfg2;
    cfg2.perf_mode = true; cfg2.apply_perf_mode();
    cfg2.use_database = false; cfg2.system_domain = "test.local";
    cfg2.storage.local.mail_path = test_support::temp_path("imaps_uidnext_test_mail");
    cfg2.storage.local.attachment_path = test_support::temp_path("imaps_uidnext_test_att");
    auto server = std::shared_ptr<TestServer>(new TestServer(cfg2, io, wk, router));
    auto fsm = std::make_shared<TraditionalImapsFsm<MockConnection>>(io, wk, router);

    // read 的 async_query 弹出一个 {uidnext: 42}；advance 的 async_execute 不消耗
    db->mock_conn()->push_sync_result(std::make_shared<test::MockDbResult>(
        std::vector<std::map<std::string, std::string>>{{{"uidnext", "42"}}}));
    auto conn = db->acquire_connection();
    uint64_t v = 0;
    fsm->get_mailbox_uidnext_async(conn, 5001, 1001, [&v](uint64_t x) { v = x; });
    assert(v == 42);
    std::cout << "  [PASS] uidnext_advance_read (uidnext=" << v << ")" << std::endl;
}

TEST(uidnext_no_db_default) {
    // 无 DB（fixture 的空 router）→ 优雅回调 1，不崩溃
    auto conn = fx.fsm->acquire_connection(0);
    uint64_t v = 0;
    fx.fsm->get_mailbox_uidnext_async(conn, 5001, 1001, [&v](uint64_t x) { v = x; });
    assert(v == 1);
    std::cout << "  [PASS] uidnext_no_db_default" << std::endl;
}

TEST(stats_flight_dedup_concurrent) {
    // 剧情：两个连接并发 SELECT 同一邮箱，缓存 miss。single-flight 应只让一个
    // 查询链在途（第二个挂到等待列表，不再发查询），owner 完成后两者都拿到结果
    // —— 防 N 连接各自查库打爆数据库，也防等待者永久挂起。
    auto io = std::make_shared<pr::IOThreadPool>(1); io->start();
    auto wk = std::make_shared<pr::WorkerThreadPool>(2); wk->start();
    auto db = std::make_shared<test::MockDbPool>();
    db->mock_conn()->set_deferred(true);   // 延迟模式：查询挂起，手动触发
    auto router = std::make_shared<router::StaticShardRouter>(
        std::vector<std::pair<std::string, int>>{},
        0,
        std::vector<std::shared_ptr<DBPool>>{db},
        std::vector<std::shared_ptr<storage::IStorageProvider>>{});
    ServerConfig cfg3;
    cfg3.perf_mode = true; cfg3.apply_perf_mode();
    cfg3.use_database = false; cfg3.system_domain = "test.local";
    cfg3.storage.local.mail_path = test_support::temp_path("imaps_flight_test_mail");
    cfg3.storage.local.attachment_path = test_support::temp_path("imaps_flight_test_att");
    auto server = std::shared_ptr<TestServer>(new TestServer(cfg3, io, wk, router));
    auto fsm = std::make_shared<TraditionalImapsFsm<MockConnection>>(io, wk, router);
    fsm->set_mailbox_stats_cache(
        std::make_shared<TraditionalImapsFsm<MockConnection>::MailboxStatsCache>(16));

    auto conn = db->acquire_connection();

    // 两个并发调用（同 key），缓存 miss
    MailboxCacheEntry r1, r2;
    bool cb1_done = false, cb2_done = false;
    fsm->get_mailbox_stats_cached_async(conn, 1001, 5001,
        [&](MailboxCacheEntry e, bool, bool) { r1 = e; cb1_done = true; });
    fsm->get_mailbox_stats_cached_async(conn, 1001, 5001,
        [&](MailboxCacheEntry e, bool, bool) { r2 = e; cb2_done = true; });

    // 关键断言：只有 owner 发出了 count 查询；第二个连接没发新查询（dedup）
    assert(db->mock_conn()->has_pending_query());
    assert(!cb1_done && !cb2_done);   // 两者都还在等待 owner 完成

    // 逐步触发 owner 链：count → unseen → advance(execute, 不消耗结果) → read
    db->mock_conn()->fire_query(std::make_shared<test::MockDbResult>(
        std::vector<std::map<std::string, std::string>>{{{"cnt", "5"}}}));
    assert(!cb1_done && !cb2_done);
    db->mock_conn()->fire_query(std::make_shared<test::MockDbResult>(
        std::vector<std::map<std::string, std::string>>{{{"cnt", "3"}}}));
    db->mock_conn()->fire_execute(true);
    db->mock_conn()->fire_query(std::make_shared<test::MockDbResult>(
        std::vector<std::map<std::string, std::string>>{{{"uidnext", "42"}}}));

    // 完成后 owner + waiter 都拿到同一份结果，无一挂起
    assert(cb1_done && cb2_done);
    assert(r1.exists == 5 && r2.exists == 5);
    assert(r1.unseen == 3 && r2.unseen == 3);
    assert(r1.uidnext == 42 && r2.uidnext == 42);

    // 缓存已写回：下一次直接命中，不再查库
    db->mock_conn()->clear_pending();
    MailboxCacheEntry r3;
    bool cb3_done = false;
    fsm->get_mailbox_stats_cached_async(conn, 1001, 5001,
        [&](MailboxCacheEntry e, bool, bool) { r3 = e; cb3_done = true; });
    assert(cb3_done && r3.exists == 5 && r3.uidnext == 42);
    assert(!db->mock_conn()->has_pending_query());   // 缓存命中，未发查询

    std::cout << "  [PASS] stats_flight_dedup_concurrent (1 query, both resumed, cache hit)" << std::endl;
}

TEST(stats_flight_concurrent_threads) {
    // 剧情：两个 OS 线程同时 miss 同一邮箱 key（模拟两个连接并发 SELECT）。
    // single-flight 保证只发一次查询链（谁拿 owner 不定），另一个挂等待列表，
    // 两个线程都拿到结果。TSan 下验证 flight 锁无数据竞争。
    auto io = std::make_shared<pr::IOThreadPool>(1); io->start();
    auto wk = std::make_shared<pr::WorkerThreadPool>(2); wk->start();
    auto db = std::make_shared<test::MockDbPool>();
    db->mock_conn()->set_deferred(true);
    auto router = std::make_shared<router::StaticShardRouter>(
        std::vector<std::pair<std::string, int>>{},
        0,
        std::vector<std::shared_ptr<DBPool>>{db},
        std::vector<std::shared_ptr<storage::IStorageProvider>>{});
    ServerConfig cfg4;
    cfg4.perf_mode = true; cfg4.apply_perf_mode();
    cfg4.use_database = false; cfg4.system_domain = "test.local";
    cfg4.storage.local.mail_path = test_support::temp_path("imaps_flight_test_mail2");
    cfg4.storage.local.attachment_path = test_support::temp_path("imaps_flight_test_att2");
    auto server = std::shared_ptr<TestServer>(new TestServer(cfg4, io, wk, router));
    auto fsm = std::make_shared<TraditionalImapsFsm<MockConnection>>(io, wk, router);
    fsm->set_mailbox_stats_cache(
        std::make_shared<TraditionalImapsFsm<MockConnection>::MailboxStatsCache>(16));
    auto conn = db->acquire_connection();

    MailboxCacheEntry ra, rb;
    bool a_done = false, b_done = false;
    std::atomic<bool> go{false};
    std::thread ta([&] {
        while (!go.load(std::memory_order_acquire)) {}
        fsm->get_mailbox_stats_cached_async(conn, 1001, 5001,
            [&](MailboxCacheEntry e, bool, bool) { ra = e; a_done = true; });
    });
    std::thread tb([&] {
        while (!go.load(std::memory_order_acquire)) {}
        fsm->get_mailbox_stats_cached_async(conn, 1001, 5001,
            [&](MailboxCacheEntry e, bool, bool) { rb = e; b_done = true; });
    });
    go.store(true, std::memory_order_release);
    ta.join();
    tb.join();

    // 两个线程都到达后：只有 owner 发出了 count 查询（去重生效）
    assert(db->mock_conn()->has_pending_query());
    assert(!a_done && !b_done);

    // 触发 owner 链 → 两个 waiter 都恢复
    db->mock_conn()->fire_query(std::make_shared<test::MockDbResult>(
        std::vector<std::map<std::string, std::string>>{{{"cnt", "7"}}}));
    db->mock_conn()->fire_query(std::make_shared<test::MockDbResult>(
        std::vector<std::map<std::string, std::string>>{{{"cnt", "2"}}}));
    db->mock_conn()->fire_execute(true);
    db->mock_conn()->fire_query(std::make_shared<test::MockDbResult>(
        std::vector<std::map<std::string, std::string>>{{{"uidnext", "99"}}}));
    assert(a_done && b_done);
    assert(ra.exists == 7 && rb.exists == 7);
    assert(ra.unseen == 2 && rb.unseen == 2);
    assert(ra.uidnext == 99 && rb.uidnext == 99);

    std::cout << "  [PASS] stats_flight_concurrent_threads (2 threads, 1 query, no hang)" << std::endl;
}

// ========== 2026-10-21 四缺陷回归 ==========

// rfc822 探针邮件（与生产探针同构）：multipart/mixed，part1=text/plain，
// part2=message/rfc822 附件 draft.eml（内嵌单叶子）。
static const std::string kProbeRfc822Body =
    "From: probe@scut.email\r\n"
    "To: leger@scut.email\r\n"
    "Subject: rfc822 probe\r\n"
    "Message-ID: <probe-rfc822-2@scut.email>\r\n"
    "Date: Mon, 20 Oct 2026 18:00:00 +0000\r\n"
    "MIME-Version: 1.0\r\n"
    "Content-Type: multipart/mixed; boundary=\"OUTER\"\r\n"
    "\r\n"
    "--OUTER\r\n"
    "Content-Type: text/plain; charset=utf-8\r\n"
    "\r\n"
    "part-one\r\n"
    "--OUTER\r\n"
    "Content-Type: message/rfc822; name=\"draft.eml\"\r\n"
    "Content-Disposition: attachment; filename=\"draft.eml\"\r\n"
    "Content-Transfer-Encoding: 8bit\r\n"
    "\r\n"
    "Message-ID: <draft-inner@scut.email>\r\n"
    "From: drafter@scut.email\r\n"
    "To: leger@scut.email\r\n"
    "Subject: inner draft\r\n"
    "Date: Mon, 20 Oct 2026 17:00:00 +0000\r\n"
    "Content-Type: text/plain; charset=utf-8\r\n"
    "\r\n"
    "inner body line\r\n"
    "--OUTER--\r\n";

// 按消费者协议从响应里抠出某个标签后的 literal 数据
static std::string fetch_literal_of(const std::string& w, const std::string& label) {
    auto start = w.find(label + " {");
    assert(start != std::string::npos);
    start += label.size() + 2;
    auto end = w.find("}\r\n", start);
    assert(end != std::string::npos);
    auto count = std::stoull(w.substr(start, end - start));
    assert(end + 3 + count <= w.size());
    return w.substr(end + 3, count);
}

// 缺陷1 端到端：message/rfc822 部件的 section 取址 + BODYSTRUCTURE
TEST(fetch_rfc822_sections) {
    auto io = std::make_shared<pr::IOThreadPool>(1); io->start();
    auto wk = std::make_shared<pr::WorkerThreadPool>(2); wk->start();
    auto db = std::make_shared<test::MockDbPool>();
    db->mock_conn()->set_deferred(false);
    namespace st = mail_system::storage;
    std::filesystem::create_directories(test_support::temp_path("imaps_rfc822_test"));
    auto storage = std::make_shared<st::LocalFileStorageProvider>(
        test_support::temp_path("imaps_rfc822_test/"), test_support::temp_path("imaps_rfc822_test/"));
    auto router2 = std::make_shared<router::StaticShardRouter>(
        std::vector<std::pair<std::string, int>>{},
        0,
        std::vector<std::shared_ptr<DBPool>>{db},
        std::vector<std::shared_ptr<st::IStorageProvider>>{storage});
    ServerConfig cfg2;
    cfg2.perf_mode = true; cfg2.apply_perf_mode();
    cfg2.use_database = false; cfg2.system_domain = "test.local";
    cfg2.storage.local.mail_path = test_support::temp_path("imaps_rfc822_test");
    cfg2.storage.local.attachment_path = test_support::temp_path("imaps_rfc822_test");
    auto server2 = std::shared_ptr<TestServer>(new TestServer(cfg2, io, wk, router2));
    auto fsm2 = std::make_shared<TraditionalImapsFsm<MockConnection>>(io, wk, router2);

    const std::string body_path = test_support::temp_path("imaps_rfc822_test/mail_probe");
    { std::ofstream f(body_path, std::ios::binary); f << kProbeRfc822Body; }

    db->mock_conn()->push_sync_result(std::make_shared<test::MockDbResult>(
        std::vector<std::map<std::string, std::string>>{{
            {"id", "77"}, {"sender", "probe@scut.email"}, {"recipient", "leger@scut.email"},
            {"subject", "rfc822 probe"}, {"body_path", body_path},
            {"is_starred", "0"}, {"is_deleted", "0"}, {"is_important", "0"},
            {"status", "1"}, {"send_time", "1700000000"}}}));

    auto conn_u = std::make_unique<MockConnection>();
    auto* conn_ptr = conn_u.get();
    auto session = std::make_shared<ImapsSession<MockConnection>>(server2.get(), std::move(conn_u), fsm2);
    auto* c = static_cast<ImapContext*>(session->get_context());
    c->is_authenticated = true; c->user_id = 1; c->mailbox_selected = true;
    c->selected_mailbox_id = 1; c->current_tag = "F1";
    session->set_current_state(static_cast<int>(ImapState::SELECTED));

    session->handle_read("F1 FETCH 1 (UID BODY.PEEK[1] BODY.PEEK[2.MIME] BODY.PEEK[2.HEADER] "
                         "BODY.PEEK[2.1] BODY.PEEK[2.TEXT] BODY.PEEK[2] BODYSTRUCTURE)\r\n");
    session->process_read();
    for (int waited = 0; waited < 2000 && conn_ptr->written().find(
            "F1 OK FETCH completed") == std::string::npos; waited += 5) {
        conn_ptr->pump_executor();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    conn_ptr->pump_executor();
    auto w = conn_ptr->written();
    assert(w.find("F1 OK FETCH completed") != std::string::npos);

    // section 取址矩阵（此前全部只有 UID 行、无数据）
    assert(fetch_literal_of(w, "BODY[1]") == "part-one");
    assert(fetch_literal_of(w, "BODY[2.MIME]").find(
        "Content-Type: message/rfc822; name=\"draft.eml\"") != std::string::npos);
    assert(fetch_literal_of(w, "BODY[2.HEADER]").find(
        "Message-ID: <draft-inner@scut.email>") != std::string::npos);
    assert(fetch_literal_of(w, "BODY[2.HEADER]").find("inner body line") == std::string::npos);
    assert(fetch_literal_of(w, "BODY[2.1]") == "inner body line");
    assert(fetch_literal_of(w, "BODY[2.TEXT]") == "inner body line");
    assert(fetch_literal_of(w, "BODY[2]").find("Message-ID: <draft-inner@scut.email>")
           != std::string::npos);

    // BODYSTRUCTURE：rfc822 部件带 envelope + 内嵌结构 + 行数
    auto bs_start = w.find("BODYSTRUCTURE ");
    assert(bs_start != std::string::npos);
    std::string bs = w.substr(bs_start);
    assert(bs.find("(\"message\" \"rfc822\" (\"NAME\" \"draft.eml\")") != std::string::npos);
    assert(bs.find("\"<draft-inner@scut.email>\"") != std::string::npos);
    assert(bs.find("\"inner draft\"") != std::string::npos);
    assert(bs.find(") (\"text\" \"plain\" (\"CHARSET\" \"utf-8\") NIL NIL \"7bit\" 15 1")
           != std::string::npos);

    std::filesystem::remove_all(test_support::temp_path("imaps_rfc822_test"));
    io->stop(); wk->stop();
    finish_session(session);
    std::cout << "  [PASS] fetch_rfc822_sections (MIME/HEADER/1/TEXT/2 all return data)" << std::endl;
}

// 缺陷4：SEARCH HEADER 判据 —— 命中/不命中/字段不存在/大小写混排
TEST(search_header_criteria) {
    auto io = std::make_shared<pr::IOThreadPool>(1); io->start();
    auto wk = std::make_shared<pr::WorkerThreadPool>(2); wk->start();
    auto db = std::make_shared<test::MockDbPool>();
    db->mock_conn()->set_deferred(false);
    namespace st = mail_system::storage;
    std::filesystem::create_directories(test_support::temp_path("imaps_search_test"));
    auto storage = std::make_shared<st::LocalFileStorageProvider>(
        test_support::temp_path("imaps_search_test/"), test_support::temp_path("imaps_search_test/"));
    auto router2 = std::make_shared<router::StaticShardRouter>(
        std::vector<std::pair<std::string, int>>{},
        0,
        std::vector<std::shared_ptr<DBPool>>{db},
        std::vector<std::shared_ptr<st::IStorageProvider>>{storage});
    ServerConfig cfg2;
    cfg2.perf_mode = true; cfg2.apply_perf_mode();
    cfg2.use_database = false; cfg2.system_domain = "test.local";
    cfg2.storage.local.mail_path = test_support::temp_path("imaps_search_test");
    cfg2.storage.local.attachment_path = test_support::temp_path("imaps_search_test");
    auto server2 = std::shared_ptr<TestServer>(new TestServer(cfg2, io, wk, router2));
    auto fsm2 = std::make_shared<TraditionalImapsFsm<MockConnection>>(io, wk, router2);

    // 3 封邮件：仅 mail 2002 携带目标 Message-ID
    const char* bodies[3] = {
        "Subject: unrelated one\r\nMessage-ID: <other-alpha@scut.email>\r\n\r\nb1\r\n",
        "Subject: sent-copy probe\r\nMESSAGE-ID: <PROBE-RFC822-2@scut.email>\r\n"
        "X-Probe: say \"probe\" at C:\\mail\r\nX-Flag: UNSEEN\r\n"
        "X-Range: UID 9999:*\r\n\r\nb2\r\n",
        "Subject: no msgid here\r\n\r\nb3\r\n",
    };
    std::vector<std::map<std::string, std::string>> rows;
    for (int i = 0; i < 3; ++i) {
        auto bp = test_support::temp_path("imaps_search_test/mail_") + std::to_string(2001 + i);
        { std::ofstream f(bp, std::ios::binary); f << bodies[i]; }
        rows.push_back({{"id", std::to_string(2001 + i)}, {"sender", "a@t.local"},
            {"recipient", "b@t.local"}, {"subject", "s"}, {"body_path", bp},
            {"is_starred", "0"}, {"is_deleted", "0"}, {"is_important", "0"},
            {"status", i == 1 ? "0" : "1"}, {"send_time", "1700000000"}});
    }
    auto push_rows = [&]() {
        db->mock_conn()->push_sync_result(
            std::make_shared<test::MockDbResult>(rows));
    };

    auto run_search = [&](const std::string& line) {
        push_rows();
        auto conn_u = std::make_unique<MockConnection>();
        auto* conn_ptr = conn_u.get();
        conn_ptr->set_deferred_read(true);
        auto session = std::make_shared<ImapsSession<MockConnection>>(
            server2.get(), std::move(conn_u), fsm2);
        auto* c = static_cast<ImapContext*>(session->get_context());
        c->is_authenticated = true; c->user_id = 1; c->mailbox_selected = true;
        c->selected_mailbox_id = 1; c->current_tag = "S";
        session->set_current_state(static_cast<int>(ImapState::SELECTED));
        session->handle_read(line);
        session->process_read();
        for (int waited = 0; waited < 2000 && conn_ptr->written().find(
                "OK SEARCH completed") == std::string::npos; waited += 5) {
            conn_ptr->pump_executor();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        conn_ptr->pump_executor();
        auto w = conn_ptr->written();
        finish_session(session);
        return w;
    };

    // 命中：只返回 2002（修复前：判据被忽略 → 返回全箱 UID）
    auto w = run_search("S1 UID SEARCH HEADER Message-ID \"<probe-rfc822-2@scut.email>\"\r\n");
    assert(w.find("* SEARCH 2002\r\n") != std::string::npos);
    assert(w.find("2001") == std::string::npos);
    assert(w.find("2003") == std::string::npos);

    // 大小写混排：字段名/值大小写不敏感
    w = run_search("S2 UID SEARCH HEADER message-ID \"probe-rfc822-2@SCUT.email\"\r\n");
    assert(w.find("* SEARCH 2002\r\n") != std::string::npos);

    // 子串匹配
    w = run_search("S3 UID SEARCH HEADER Subject probe\r\n");
    assert(w.find("* SEARCH 2002\r\n") != std::string::npos);
    assert(w.find("2001") == std::string::npos && w.find("2003") == std::string::npos);

    // 头不存在 → 空
    w = run_search("S4 UID SEARCH HEADER X-Nope anything\r\n");
    assert(w.find("* SEARCH\r\n") != std::string::npos);

    // 既有判据不回归：目标邮件已读，另外两封未读。
    w = run_search("S5 UID SEARCH UNSEEN\r\n");
    assert(w.find("2001") != std::string::npos && w.find("2002") == std::string::npos
           && w.find("2003") != std::string::npos);

    w = run_search("S6 UID SEARCH HEADER Subject \"sent-copy probe\"\r\n");
    assert(w.find("* SEARCH 2002\r\n") != std::string::npos);
    w = run_search("S7 SEARCH HEADER Subject \"sent-copy probe\"\r\n");
    assert(w.find("* SEARCH 2\r\n") != std::string::npos);
    w = run_search("S8 UID SEARCH HEADER Subject \"sent-copy missing\"\r\n");
    assert(w.find("* SEARCH\r\n") != std::string::npos);
    w = run_search("S9 UID SEARCH HEADER X-Probe \"say \\\"probe\\\" at C:\\\\mail\"\r\n");
    assert(w.find("* SEARCH 2002\r\n") != std::string::npos);
    std::cout << "  [PASS] search_header_quoted_values (multiword hit/miss/escaped quote/backslash)" << std::endl;

    // HEADER 的值必须作为参数消费，不能再被当作 flag/UID 判据。
    w = run_search("S10 UID SEARCH HEADER X-Flag \"UNSEEN\"\r\n");
    assert(w.find("* SEARCH 2002\r\n") != std::string::npos);
    w = run_search("S11 UID SEARCH HEADER X-Range \"UID 9999:*\"\r\n");
    assert(w.find("* SEARCH 2002\r\n") != std::string::npos);
    w = run_search("S12 UID SEARCH HEADER X-Flag \"\" SEEN\r\n");
    assert(w.find("* SEARCH 2002\r\n") != std::string::npos);

    // Linux APPEND 的旧数据：文件存在，mails.body_path 却为空（参数求值时被移动）。
    const std::string legacy_key = mail_system::storage::build_mail_body_key(storage, 2002);
    std::filesystem::copy_file(rows[1]["body_path"], legacy_key,
                               std::filesystem::copy_options::overwrite_existing);
    rows[1]["body_path"].clear();
    w = run_search("S13 UID SEARCH HEADER Subject \"sent-copy probe\"\r\n");
    assert(w.find("* SEARCH 2002\r\n") != std::string::npos);
    w = run_search("S14 UID SEARCH HEADER Subject \"sent-copy missing\"\r\n");
    assert(w.find("* SEARCH\r\n") != std::string::npos);
    w = run_search("S15 UID SEARCH UID 2002:2003 HEADER Subject \"sent-copy probe\" SEEN\r\n");
    assert(w.find("* SEARCH 2002\r\n") != std::string::npos);
    w = run_search("S16 UID SEARCH UID 2003:* HEADER Subject \"sent-copy probe\"\r\n");
    assert(w.find("* SEARCH\r\n") != std::string::npos);
    w = run_search("S17 UID SEARCH HEADER Subject \"sent-copy probe\" HEADER X-Probe missing\r\n");
    assert(w.find("* SEARCH\r\n") != std::string::npos);

    std::filesystem::remove_all(test_support::temp_path("imaps_search_test"));
    io->stop(); wk->stop();
    std::cout << "  [PASS] search_header_criteria (hit/miss/case/absent/UNSEEN intact)" << std::endl;
}

TEST(append_metadata_body_path) {
    struct InlineWorkers final : pr::ThreadPoolBase {
        void start() override {}
        void stop(bool = true) override {}
        size_t thread_count() const override { return 1; }
        bool is_running() const override { return true; }
        void post_impl(std::function<void()> task) override { task(); }
    };
    auto db = std::make_shared<test::MockDbPool>();
    auto conn = db->acquire_connection();
    const std::string root = test_support::temp_path("imaps_append_metadata/");
    std::filesystem::create_directories(root);
    auto storage = std::make_shared<storage::LocalFileStorageProvider>(root, root);
    auto service = make_imap_append_service(std::make_shared<InlineWorkers>());
    ImapAppendResult output;
    service->append_async(conn, storage, {41, 7, "sent-copy probe", "Subject: sent-copy probe\r\n\r\nbody", "owner@test", 0},
        [&](ImapAppendResult result) { output = std::move(result); });
    assert(db->mock_conn()->has_pending_execute());
    const auto params = db->mock_conn()->last_query_params();
    assert(params.size() == 4);
    const std::string key = mail_system::storage::build_mail_body_key(storage, std::stoull(params[0]));
    assert(params[2] == key && std::filesystem::exists(key));
    db->mock_conn()->fire_execute(true);
    db->mock_conn()->fire_query(std::make_shared<test::MockDbResult>());
    db->mock_conn()->fire_execute(true);
    db->mock_conn()->fire_execute(true);
    assert(output.ok && output.mail_id == std::stoull(params[0]) && output.body_key == key);
    std::filesystem::remove_all(root);
    std::cout << "  [PASS] append_metadata_body_path (SQL path == storage key == callback path)" << std::endl;
}

// 缺陷2：APPEND DB 写失败 → tagged NO、无孤儿体文件、无成功日志（OK APPENDUID）
TEST(append_db_fail_no_orphan) {
    auto io = std::make_shared<pr::IOThreadPool>(1); io->start();
    auto wk = std::make_shared<pr::WorkerThreadPool>(2); wk->start();
    auto db = std::make_shared<test::MockDbPool>();
    db->mock_conn()->set_deferred(false);
    namespace st = mail_system::storage;
    std::filesystem::create_directories(test_support::temp_path("imaps_append_fail/mail"));
    auto storage = std::make_shared<st::LocalFileStorageProvider>(
        test_support::temp_path("imaps_append_fail/mail/"), test_support::temp_path("imaps_append_fail/att/"));
    auto router2 = std::make_shared<router::StaticShardRouter>(
        std::vector<std::pair<std::string, int>>{},
        0,
        std::vector<std::shared_ptr<DBPool>>{db},
        std::vector<std::shared_ptr<st::IStorageProvider>>{storage});
    ServerConfig cfg2;
    cfg2.perf_mode = true; cfg2.apply_perf_mode();
    cfg2.use_database = false; cfg2.system_domain = "test.local";
    cfg2.storage.local.mail_path = test_support::temp_path("imaps_append_fail/mail");
    cfg2.storage.local.attachment_path = test_support::temp_path("imaps_append_fail/att");
    auto server2 = std::shared_ptr<TestServer>(new TestServer(cfg2, io, wk, router2));
    auto fsm2 = std::make_shared<TraditionalImapsFsm<MockConnection>>(io, wk, router2);

    // 查询剧本：找邮箱（INBOX id=9）→ user_email 留空（回调兜底 username）
    db->mock_conn()->push_sync_result(std::make_shared<test::MockDbResult>(
        std::vector<std::map<std::string, std::string>>{{{"id", "9"}}}));
    // 执行剧本：mails ✓ → mail_recipients ✓ → mail_mailbox ✗（生产 add_time 症状）
    // → 回滚三连 ✓
    db->mock_conn()->push_execute_result(true);
    db->mock_conn()->push_execute_result(true);
    db->mock_conn()->push_execute_result(false);
    db->mock_conn()->push_execute_result(true);
    db->mock_conn()->push_execute_result(true);
    db->mock_conn()->push_execute_result(true);

    auto conn_u = std::make_unique<MockConnection>();
    auto* conn_ptr = conn_u.get();
    conn_ptr->set_deferred_read(true);   // 防 do_async_read→EOF→close 杀死会话
    auto session = std::make_shared<ImapsSession<MockConnection>>(server2.get(), std::move(conn_u), fsm2);
    auto* c = static_cast<ImapContext*>(session->get_context());
    c->is_authenticated = true; c->user_id = 7; c->current_tag = "A1";
    session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));

    // literal 续行路径（生产时序：先声明 → + → 数据；数据分两段到达）
    session->handle_read("A1 APPEND INBOX {5}\r\n");
    session->process_read();   // literal 声明阶段：只发 continuation，不分发
    conn_ptr->pump_executor();
    session->handle_read("hel");        // 3 < 5：继续累积
    session->handle_read("lo12\r\n");   // 凑满 5 → args="hello"，多余 CRLF 留缓冲
    session->process_read();   // 分发 APPEND

    for (int waited = 0; waited < 2000 && conn_ptr->written().find("A1 ") == std::string::npos;
         waited += 5) {
        conn_ptr->pump_executor();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    conn_ptr->pump_executor();
    auto w = conn_ptr->written();
    assert(w.find("A1 NO [SERVERBUG] APPEND failed") != std::string::npos);
    assert(w.find("OK [APPENDUID") == std::string::npos);   // 失败不得回 OK

    // 无孤儿体文件
    int leftover = 0;
    for (const auto& e : std::filesystem::directory_iterator(test_support::temp_path("imaps_append_fail/mail")))
        leftover++;
    assert(leftover == 0);

    std::filesystem::remove_all(test_support::temp_path("imaps_append_fail"));
    io->stop(); wk->stop();
    finish_session(session);
    std::cout << "  [PASS] append_db_fail_no_orphan (tagged NO, no OK, no file)" << std::endl;
}

// 缺陷3：APPEND 失败后同会话 SELECT 必须有响应（生产 30s 挂死回归）
TEST(append_fail_then_select_responds) {
    auto io = std::make_shared<pr::IOThreadPool>(1); io->start();
    auto wk = std::make_shared<pr::WorkerThreadPool>(2); wk->start();
    auto db = std::make_shared<test::MockDbPool>();
    db->mock_conn()->set_deferred(false);
    namespace st = mail_system::storage;
    std::filesystem::create_directories(test_support::temp_path("imaps_append_sel/mail"));
    auto storage = std::make_shared<st::LocalFileStorageProvider>(
        test_support::temp_path("imaps_append_sel/mail/"), test_support::temp_path("imaps_append_sel/att/"));
    auto router2 = std::make_shared<router::StaticShardRouter>(
        std::vector<std::pair<std::string, int>>{},
        0,
        std::vector<std::shared_ptr<DBPool>>{db},
        std::vector<std::shared_ptr<st::IStorageProvider>>{storage});
    ServerConfig cfg2;
    cfg2.perf_mode = true; cfg2.apply_perf_mode();
    cfg2.use_database = false; cfg2.system_domain = "test.local";
    cfg2.storage.local.mail_path = test_support::temp_path("imaps_append_sel/mail");
    cfg2.storage.local.attachment_path = test_support::temp_path("imaps_append_sel/att");
    auto server2 = std::shared_ptr<TestServer>(new TestServer(cfg2, io, wk, router2));
    auto fsm2 = std::make_shared<TraditionalImapsFsm<MockConnection>>(io, wk, router2);
    // SELECT 统计链有缓存，测试隔离：禁用缓存直接回源
    fsm2->set_mailbox_stats_cache(nullptr);

    auto conn_u = std::make_unique<MockConnection>();
    auto* conn_ptr = conn_u.get();
    conn_ptr->set_deferred_read(true);   // 防 do_async_read→EOF→close 杀死会话
    auto session = std::make_shared<ImapsSession<MockConnection>>(server2.get(), std::move(conn_u), fsm2);
    auto* c = static_cast<ImapContext*>(session->get_context());
    c->is_authenticated = true; c->user_id = 7; c->current_tag = "A1";
    session->set_current_state(static_cast<int>(ImapState::SELECTED));

    // —— APPEND：关联表写失败（第三个 execute）→ NO ——
    db->mock_conn()->push_sync_result(std::make_shared<test::MockDbResult>(
        std::vector<std::map<std::string, std::string>>{{{"id", "9"}}}));   // 找邮箱
    db->mock_conn()->push_execute_result(true);    // mails
    db->mock_conn()->push_execute_result(true);    // mail_recipients
    db->mock_conn()->push_execute_result(false);   // mail_mailbox ✗
    db->mock_conn()->push_execute_result(true);    // 回滚 ×3
    db->mock_conn()->push_execute_result(true);
    db->mock_conn()->push_execute_result(true);

    session->handle_read("A1 APPEND INBOX {5}\r\n");
    session->process_read();
    conn_ptr->pump_executor();
    session->handle_read("12345\r\n");
    session->process_read();
    for (int waited = 0; waited < 2000 && conn_ptr->written().find(
            "A1 NO [SERVERBUG] APPEND failed") == std::string::npos; waited += 5) {
        conn_ptr->pump_executor();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(conn_ptr->written().find("A1 NO [SERVERBUG] APPEND failed") != std::string::npos);

    // —— 同连接 SELECT（SELECTED 态重选，缺陷3 主回归）——
    db->mock_conn()->push_sync_result(std::make_shared<test::MockDbResult>(
        std::vector<std::map<std::string, std::string>>{{{"id", "9"}}}));   // 找邮箱
    db->mock_conn()->push_sync_result(std::make_shared<test::MockDbResult>(
        std::vector<std::map<std::string, std::string>>{{{"cnt", "3"}}}));  // count
    db->mock_conn()->push_sync_result(std::make_shared<test::MockDbResult>(
        std::vector<std::map<std::string, std::string>>{{{"cnt", "1"}}}));  // unseen
    db->mock_conn()->push_sync_result(std::make_shared<test::MockDbResult>(
        std::vector<std::map<std::string, std::string>>{{{"uidnext", "42"}}}));  // uidnext
    session->handle_read("A2 SELECT INBOX\r\n");
    session->process_read();
    bool responded = false;
    for (int waited = 0; waited < 2000 && !responded; waited += 5) {
        conn_ptr->pump_executor();
        responded = conn_ptr->written().find("A2 OK [READ-WRITE] SELECT completed") != std::string::npos;
        if (!responded) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(responded);   // 修复前：BAD Invalid command sequence / 无响应挂死

    std::filesystem::remove_all(test_support::temp_path("imaps_append_sel"));
    io->stop(); wk->stop();
    finish_session(session);
    std::cout << "  [PASS] append_fail_then_select_responds (SELECTED-state re-select)" << std::endl;
}

// Session 级回归：所有输入经 do_async_read/MockConnection 分块投递，不能直呼
// handle_read/process_read 绕过读重武装。DB/存储走真实 APPEND 异步 worker 链。
TEST(append_success_then_select_responds) {
    auto io = std::make_shared<pr::IOThreadPool>(1); io->start();
    auto wk = std::make_shared<pr::WorkerThreadPool>(2); wk->start();
    auto db = std::make_shared<test::MockDbPool>();
    db->mock_conn()->set_deferred(false);
    namespace st = mail_system::storage;
    std::filesystem::create_directories(test_support::temp_path("imaps_append_ok/mail"));
    auto storage = std::make_shared<st::LocalFileStorageProvider>(
        test_support::temp_path("imaps_append_ok/mail/"), test_support::temp_path("imaps_append_ok/att/"));
    auto router2 = std::make_shared<router::StaticShardRouter>(
        std::vector<std::pair<std::string, int>>{}, 0,
        std::vector<std::shared_ptr<DBPool>>{db},
        std::vector<std::shared_ptr<st::IStorageProvider>>{storage});
    ServerConfig cfg2;
    cfg2.perf_mode = true; cfg2.apply_perf_mode();
    cfg2.use_database = false; cfg2.system_domain = "test.local";
    cfg2.storage.local.mail_path = test_support::temp_path("imaps_append_ok/mail");
    cfg2.storage.local.attachment_path = test_support::temp_path("imaps_append_ok/att");
    auto server2 = std::make_shared<TestServer>(cfg2, io, wk, router2);
    auto fsm2 = std::make_shared<TraditionalImapsFsm<MockConnection>>(io, wk, router2);
    fsm2->set_mailbox_stats_cache(nullptr);
    AuthCacheEntry auth;
    auth.password_hash = "test123"; auth.user_id = 7;
    fsm2->m_authCache->inject("user@test.local", auth);

    // 同段尾符覆盖 pending_write_buf_ 冲刷；分段尾符覆盖 APPEND OK 后才到达的
    // 孤立 CRLF；literal 自身也可以跨多次读取，CR/LF 可以分别到达。
    const std::vector<std::vector<std::string>> chunks = {
        {"12345\r\n"},
        {"12345", "\r\n"},
        {"12", "345", "\r", "\n"},
        {"12345\r\n"}, // 最后一轮还延迟实际写完成，覆盖暂存响应 flush 的回调。
    };
    size_t case_index = 0;
    for (const auto& parts : chunks) {
        const bool slow_write = ++case_index == chunks.size();
        auto conn_u = std::make_unique<MockConnection>();
        auto* conn = conn_u.get();
        conn->set_deferred_read(true);
        auto session = std::make_shared<ImapsSession<MockConnection>>(
            server2.get(), std::move(conn_u), fsm2);
        session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));
        auto wait_for = [&](const std::string& needle) {
            for (int waited = 0; waited < 2000; waited += 5) {
                conn->pump_executor();
                if (conn->written().find(needle) != std::string::npos) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return false;
        };
        auto feed = [&](const std::string& bytes) {
            conn->pump_executor();
            assert(conn->has_pending_read());
            conn->trigger_deferred_read(bytes);
            conn->pump_executor();
        };
        session->do_async_read();
        feed("A0 LOGIN user@test.local test123\r\n");
        assert(wait_for("A0 OK LOGIN completed"));
        assert(session->get_current_state() == static_cast<int>(ImapState::AUTHENTICATED));

        db->mock_conn()->push_sync_result(std::make_shared<test::MockDbResult>(
            std::vector<std::map<std::string, std::string>>{{{"id", "9"}}}));
        db->mock_conn()->push_execute_result(true); // mails
        db->mock_conn()->push_execute_result(true); // mail_recipients
        db->mock_conn()->push_execute_result(true); // mail_mailbox
        feed("A1 APPEND INBOX {5}\r\n");
        assert(wait_for("+ Ready for literal data\r\n"));
        conn->set_deferred_write(slow_write);
        size_t literal_bytes = 0;
        for (const auto& part : parts) {
            feed(part);
            literal_bytes += part.size();
            // 分段 CRLF 明确晚于 tagged OK，确定性还原生产挂死时序。
            if (literal_bytes >= 5) assert(wait_for("A1 OK [APPENDUID 9 "));
        }
        if (slow_write) {
            assert(conn->has_pending_write());
            conn->trigger_deferred_write();
            conn->set_deferred_write(false);
        }
        conn->pump_executor();
        // 修复前：同段 CRLF 能过，独立 CRLF 消费后没有挂起读，后续 SELECT 永不派发。
        assert(conn->has_pending_read());

        db->mock_conn()->push_sync_result(std::make_shared<test::MockDbResult>(
            std::vector<std::map<std::string, std::string>>{{{"id", "9"}}}));
        db->mock_conn()->push_sync_result(std::make_shared<test::MockDbResult>(
            std::vector<std::map<std::string, std::string>>{{{"cnt", "4"}}}));
        db->mock_conn()->push_sync_result(std::make_shared<test::MockDbResult>(
            std::vector<std::map<std::string, std::string>>{{{"cnt", "2"}}}));
        db->mock_conn()->push_sync_result(std::make_shared<test::MockDbResult>(
            std::vector<std::map<std::string, std::string>>{{{"uidnext", "43"}}}));
        feed("A2 SELECT INBOX\r\n");
        assert(wait_for("A2 OK [READ-WRITE] SELECT completed"));
        feed("A3 LOGOUT\r\n");
        assert(wait_for("A3 OK LOGOUT completed"));
        finish_session(session);
    }
    std::filesystem::remove_all(test_support::temp_path("imaps_append_ok"));
    io->stop(); wk->stop();
    std::cout << "  [PASS] append_success_then_select_responds (session read chain, split literal/CRLF, SELECT/LOGOUT)" << std::endl;
}

// 缺陷3 根因：literal 之后同段到达的下一条命令不得被吞进邮件体
TEST(append_literal_excess_not_swallowed) {
    auto h = fx.make_session();
    h.conn->set_deferred_read(true);   // 防 do_async_read→EOF→close 杀死会话
    auto* c = static_cast<ImapContext*>(h.session->get_context());
    c->is_authenticated = true; c->current_tag = "X1";
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));

    // 声明 literal {5}（fixture 无 DB：APPEND 会 NO，但不影响本用例的解析断言）
    h.session->handle_read("X1 APPEND INBOX {5}\r\n");
    h.conn->pump_executor();
    assert(h.conn->written().find("+ ") != std::string::npos);   // continuation 已发

    // literal + CRLF + 下一条命令同段到达（生产挂死时序）
    h.session->handle_read("hello\r\nX2 NOOP\r\n");
    h.session->process_read();   // 分发 APPEND（NO：无 DB）
    h.conn->pump_executor();
    // 排空缓冲：CRLF（忽略）→ X2 NOOP（修复前：被吞进 args，永不派发）
    h.session->drain_buffered_commands();
    h.conn->pump_executor();
    auto w = h.conn->written();
    assert(w.find("X1 NO") != std::string::npos);
    assert(w.find("X2 OK NOOP") != std::string::npos);

    // args 恰好等于 literal：越界的 "\r\nX2 NOOP\r\n" 不在邮件体里
    // （通过已断言的 X2 NOOP 响应间接成立）
    std::cout << "  [PASS] append_literal_excess_not_swallowed (next command dispatched)" << std::endl;
}

int main() {
    std::cout << "IMAP FSM Test Suite\n==================\n";

    try {
        FsmTestFixture fx;

        // ── 基础命令 ──
        test_connection_lease_keeps_pool_after_fsm_destruction(fx);
        test_capability_response(fx);
        test_login_response(fx);
        test_login_success_real_auth(fx);
        test_login_uses_injected_authentication_service(fx);
        test_abnormal_trace_after_auth(fx);
        test_auth_plain_ir_success(fx);
        test_auth_plain_two_step(fx);
        test_auth_plain_abort(fx);
        test_auth_plain_wrong_password(fx);
        test_auth_plain_bad_base64(fx);
        test_auth_unsupported_mechanism(fx);
        test_login_wrong_password(fx);
        test_login_many_failures_close(fx);
        test_noop_reply(fx);
        test_logout_bye(fx);

        // ── 未认证拒绝 ──
        test_select_without_login(fx);
        test_fetch_without_login(fx);

        // ── UID FETCH crash repro ──
        test_uid_fetch_no_db(fx);
        test_fetch_full_path_with_storage(fx);
        test_fetch_body_part_number(fx);   // BODY.PEEK[n] 节号 off-by-one 回归（2026-09-28）
        test_fetch_many_mails_no_stack_overflow(fx);
        test_literal_too_large_rejected(fx);

        // ── UIDNEXT 高水位 ──
        test_mailbox_repository_injection(fx);
        test_mutation_repository_store_and_expunge_injection(fx);
        test_mutation_repository_copy_move_failures(fx);
        test_mutation_repository_same_mailbox_move_is_rejected(fx);
        test_mutation_repository_close_resumes_context_on_executor(fx);
        test_mutation_failures_return_no_without_false_notifications(fx);
        test_message_repository_read_facades_are_injectable(fx);
        test_folder_repository_protocol_injection(fx);
        test_append_service_protocol_injection(fx);
        test_uidnext_advance_read(fx);
        test_uidnext_no_db_default(fx);

        // ── 统计 single-flight 去重（并发 SELECT 同邮箱） ──
        test_stats_flight_dedup_concurrent(fx);
        test_stats_flight_concurrent_threads(fx);

        // ── 命令错误 ──
        test_invalid_command_in_state(fx);

        // ── 无需 DB 的命令 ──
        test_logout_without_login(fx);
        test_check_in_authenticated(fx);
        test_noop_in_selected(fx);
        test_capability_in_authenticated(fx);

        // ── DB 依赖命令（无 DB 时须优雅失败） ──
        test_select_no_db(fx);
        test_examine_no_db(fx);
        test_create_no_db(fx);
        test_delete_no_db(fx);
        test_rename_no_db(fx);
        test_subscribe_no_db(fx);
        test_unsubscribe_no_db(fx);
        test_list_no_db(fx);
        test_lsub_no_db(fx);
        test_status_no_db(fx);
        test_append_no_db(fx);
        test_search_no_db(fx);
        test_fetch_no_db(fx);
        test_store_no_db(fx);
        test_copy_no_db(fx);
        test_move_no_db(fx);
        test_expunge_no_db(fx);
        test_close_no_db(fx);

        // ── 连接/错误/超时 ──
        test_connect_greeting(fx);
        test_authenticate_not_supported(fx);
        test_error_event(fx);
        test_timeout_event(fx);

        // ── 2026-10-21 四缺陷回归 ──
        test_fetch_rfc822_sections(fx);
        test_search_header_criteria(fx);
        test_search_missing_layout_returns_no_and_resumes(fx);
        test_append_metadata_body_path(fx);
        test_append_db_fail_no_orphan(fx);
        test_append_fail_then_select_responds(fx);
        test_append_literal_excess_not_swallowed(fx);
        test_append_success_then_select_responds(fx);

        // ── IDLE/DONE ──
        test_idle_initiated(fx);
        test_idle_with_done(fx);

        // ── IDLE push（本地投递 → untagged EXISTS） ──
        test_idle_push_exists_on_delivery(fx);
        test_idle_push_ignores_session_after_done(fx);
        test_idle_push_cleans_up_destroyed_session(fx);
        test_idle_push_select_sets_exists_baseline(fx);

        std::cout << "\nAll tests passed.\n";
    } catch (const std::exception& e) {
        std::cerr << "FATAL: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
