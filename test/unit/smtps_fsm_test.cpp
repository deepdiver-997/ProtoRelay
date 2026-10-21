#include "test_temp_dir.h"
// 入站 SMTP FSM 单元测试 — MockConnection 零 I/O 验证状态转换和响应码
#undef NDEBUG
#include "no_mail_layout_storage.h"
#include <cassert>  // 在 #undef NDEBUG 后重新包含，确保 assert 生效
#include "mail_system/back/mailServer/smtps_server.h"
#include "mail_system/back/mailServer/fsm/smtps/traditional_smtps_fsm.tpp"
#include "mail_system/back/mailServer/session/smtps_session.tpp"
#include "framework/session_base.tpp"
#include "framework/server_config.h"
#include "framework/thread_pool/io_thread_pool.h"
#include "framework/thread_pool/worker_thread_pool.h"
#include "mail_system/back/persist_storage/persistent_queue.h"
#include "mail_system/back/router/static_shard_router.h"
#include "mail_system/back/storage/local_file_storage_provider.h"
#include "mail_system/back/common/logger.h"
#include "mail_system/back/common/mail_crypto.h"
#include "mail_system/back/algorithm/line_folder.h"
#include "mock_connection.h"
#include "manual_executor.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <chrono>
#include <memory>
#include <string>

using namespace mail_system;
using mail_system::algorithm::LineFolder;
namespace pq = persist_storage;

// ========== 测试夹具 ==========

// 注意：必须派生自 SmtpsServer 而非 pr::ServerBase。
// SmtpsSession 构造函数通过 `static_cast<SmtpsServer*>(server)->m_persistentQueue`
// 读取持久化队列；若 server 不是真正的 SmtpsServer，该向下转型是未定义行为，
// 会在构造时读越界垃圾指针（曾导致间歇性 SIGBUS/SIGSEGV）。
struct TestServer : SmtpsServer {
    TestServer(const ServerConfig& c,
               std::shared_ptr<pr::ThreadPoolBase> io,
               std::shared_ptr<pr::ThreadPoolBase> w,
               std::shared_ptr<router::IShardRouter> r)
        : SmtpsServer(c, io, w, std::move(r)) {}
    void start() override {}
};

struct FsmTestFixture {
    std::shared_ptr<pr::IOThreadPool> io_pool;
    std::shared_ptr<pr::WorkerThreadPool> worker_pool;
    std::shared_ptr<router::StaticShardRouter> router;
    std::shared_ptr<pq::PersistentQueue> persist_q;
    std::shared_ptr<TestServer> server;
    std::shared_ptr<TraditionalSmtpsFsm<MockConnection>> fsm;
    ServerConfig cfg;

    FsmTestFixture() {
        Logger::get_instance().init("smtps_fsm_test.log", 0, 1, pr::LogLevel::debug, false, true);
        io_pool     = std::make_shared<pr::IOThreadPool>(1);
        worker_pool = std::make_shared<pr::WorkerThreadPool>(2);
        io_pool->start();
        worker_pool->start();

        // 真实存储提供者：正文落盘到 /tmp/smtps_fsm_test_mail，供落盘内容断言
        // （此前为空列表 → 正文被静默丢弃，DATA 落盘路径零覆盖）
        auto storage = std::make_shared<storage::LocalFileStorageProvider>(
            test_support::temp_path("smtps_fsm_test_mail"), test_support::temp_path("smtps_fsm_test_att"));
        router = std::make_shared<router::StaticShardRouter>(
            std::vector<std::pair<std::string, int>>{},
            0,
            std::vector<std::shared_ptr<DBPool>>{},
            std::vector<std::shared_ptr<storage::IStorageProvider>>{storage});

        persist_q = std::make_shared<pq::PersistentQueue>(router, worker_pool);
        pq::PersistentQueuePressureConfig pc;
        pc.max_inflight_mails           = 1000000;
        pc.min_available_memory_mb      = 0;
        pc.min_db_available_connections = 0;
        persist_q->set_pressure_config(pc);
        persist_q->set_local_domain("test.local");

        cfg.perf_mode          = true;
        cfg.apply_perf_mode();
        cfg.use_database       = false;
        cfg.inbound_ack_mode   = InboundAckMode::AFTER_ENQUEUE;
        cfg.system_domain      = "test.local";
        cfg.mail_storage_path  = test_support::temp_path("smtps_fsm_test_mail");
        cfg.attachment_storage_path = test_support::temp_path("smtps_fsm_test_att");
        cfg.storage.local.mail_path       = test_support::temp_path("smtps_fsm_test_mail");
        cfg.storage.local.attachment_path = test_support::temp_path("smtps_fsm_test_att");

        std::filesystem::create_directories(test_support::temp_path("smtps_fsm_test_mail")); std::filesystem::create_directories(test_support::temp_path("smtps_fsm_test_att"));

        server = std::shared_ptr<TestServer>(new TestServer(cfg, io_pool, worker_pool, router));
        // SmtpsSession 构造函数从 server 读取持久化队列，务必绑定到测试自身的队列
        server->m_persistentQueue = persist_q;

        fsm = std::make_shared<TraditionalSmtpsFsm<MockConnection>>(
            io_pool, worker_pool, persist_q, router);

        // 预注入 auth 缓存 (user0~user4@test.local, 密码 test123)
        for (int i = 0; i < 5; ++i) {
            std::string email = "user" + std::to_string(i) + "@test.local";
            AuthCacheEntry entry;
            entry.password_hash = "test123";
            entry.status = 1;
            fsm->m_authCache->inject(email, entry);
        }

        // 预注入收件人存在性缓存（无 DB 环境，RCPT 校验走缓存路径）
        auto inject_rcpt = [this](const std::string& email, int status) {
            AuthCacheEntry entry;
            entry.password_hash = "";
            entry.status = status;
            fsm->m_recipientCache->inject(email, entry);
        };
        for (int i = 0; i < 5; ++i)
            inject_rcpt("user" + std::to_string(i) + "@test.local", 1);
        // 现有测试用到的收件人
        inject_rcpt("rcpt@test.local", 1);
        inject_rcpt("rcpt1@test.local", 1);
        inject_rcpt("rcpt2@test.local", 1);
        inject_rcpt("rcpt3@test.local", 1);
        inject_rcpt("r1@test.local", 1);
        inject_rcpt("r2@test.local", 1);
        // 不存在的本地用户（负缓存）
        inject_rcpt("nobody@test.local", 0);

        // 预注入别名缓存（RCPT 展开，无 DB 环境）：team→user1+user2 邮件组；
        // 空 vector = 确认非别名（负缓存）。别名目标必须是注入过收件人缓存的
        // 真实用户，与 build_alias_targets_query 的 JOIN users 语义一致。
        fsm->m_aliasCache->put("team@test.local",
                               {"user1@test.local", "user2@test.local"});
        fsm->m_aliasCache->put("solo@test.local", {"user0@test.local"});
        fsm->m_aliasCache->put("plain@test.local", {});
    }

    ~FsmTestFixture() {
        persist_q->shutdown();
        worker_pool->stop();
        io_pool->stop();
        std::filesystem::remove_all(test_support::temp_path("smtps_fsm_test_mail")); std::filesystem::remove_all(test_support::temp_path("smtps_fsm_test_att"));
    }

    // 创建 session，初始化 INIT 状态，返回 session + raw conn 指针
    struct Handle {
        MockConnection* conn;
        std::shared_ptr<SmtpsSession<MockConnection>> session;
        // An idle peer may leave a read holding the session. Close and drain
        // before releasing the last external owner so no callback/self cycle
        // outlives this fixture's server and shared runtimes.
        ~Handle() { finish_session(session); }
    };

    Handle make_session(const std::string& preload_data = "",
                        InboundAuthPolicy auth_policy = InboundAuthPolicy::OFF,
                        bool advertise_starttls = true) {
        auto conn_u = std::make_unique<MockConnection>();
        auto* conn_ptr = conn_u.get();
        // Exhausting a scripted input batch represents an idle, connected peer.
        // EOF is an explicit peer action (trigger_deferred_read("")), not the
        // absence of more currently available bytes while AUTH/DB work runs.
        conn_ptr->set_deferred_read(true);
        if (!preload_data.empty())
            conn_ptr->set_read_data(preload_data);

        auto session = std::make_shared<SmtpsSession<MockConnection>>(
            server.get(), std::move(conn_u),
            fsm);

        ListenerConfig lc;
        lc.type       = ListenerType::TCP;
        lc.port       = 25;
        lc.auth_policy = auth_policy;
        lc.advertise_starttls = advertise_starttls;
        session->set_listener_config(lc);
        session->set_current_state(static_cast<int>(SmtpsState::INIT));
        session->set_next_event(static_cast<int>(SmtpsEvent::CONNECT));

        return {conn_ptr, session};
    }

    // Start the FSM; the scripted peer stays connected until explicit EOF or teardown.
    void start(Handle& h) {
        h.session->process_read();
        h.conn->pump_executor();   // 响应写经 exec_ctx_ 串行化(09-06),需排空才可见
    }
};

// ========== 测试宏 ==========
// AUTH 已异步化（bcrypt/DB 链在 worker 线程）：回复可能晚于喂入返回，
// 轮询等待期望的子串出现。
template <typename Handle>
static bool wait_for_response(Handle& h, const std::string& needle, int timeout_ms = 2000) {
    for (int waited = 0; waited < timeout_ms; waited += 5) {
        h.conn->pump_executor();   // 异步回复落在 exec_ctx_ 队列,轮询需排空
        if (h.conn->written().find(needle) != std::string::npos) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    h.conn->pump_executor();
    const bool found = h.conn->written().find(needle) != std::string::npos;
    if (!found) std::cerr << "Timed out waiting for: " << needle << "\nSMTP replies:\n" << h.conn->written() << std::endl;
    return found;
}

// ========== 测试宏 ==========
#define TEST(name) \
    void test_##name(FsmTestFixture& fx)

#define HAS(str, sub) ((str).find(sub) != std::string::npos)

// ========== Happy Path ==========

TEST(greeting_220) {
    auto h = fx.make_session();
    fx.fsm->process_event(h.session, SmtpsEvent::CONNECT);
    h.conn->pump_executor();   // 同上:发起串行化后 greeting 在 exec_ctx_ 队列里
    auto w = h.conn->written();
    assert(HAS(w, "220 SMTPS Server"));
    std::cout << "  [PASS] greeting_220: " << w.substr(0, w.find('\r')) << std::endl;
}

TEST(ehlo_capabilities) {
    // 预加载 EHLO，CONNECT→greeting→do_async_read→吃 EHLO→EHLO handler
    auto h = fx.make_session("EHLO mail.test.local\r\n");
    fx.start(h);
    auto w = h.conn->written();
    assert(HAS(w, "220 SMTPS Server"));
    assert(HAS(w, "250-test.local Hello"));   // RFC 5321: EHLO 响应首行=服务器域名
    assert(HAS(w, "250-SIZE"));
    assert(HAS(w, "250-8BITMIME"));
    assert(HAS(w, "STARTTLS"));       // TCP 连接会宣告 STARTTLS
    assert(HAS(w, "250 SMTPUTF8"));
    std::cout << "  [PASS] ehlo_capabilities" << std::endl;
}

TEST(ehlo_with_auth_policy_on) {
    auto h = fx.make_session("EHLO test\r\n", InboundAuthPolicy::ON);
    fx.start(h);
    auto w = h.conn->written();
    assert(HAS(w, "AUTH LOGIN PLAIN"));  // auth_policy=ON 时宣告 AUTH
    std::cout << "  [PASS] ehlo_with_auth_policy_on" << std::endl;
}

TEST(ehlo_with_auth_policy_off) {
    auto h = fx.make_session("EHLO test\r\n", InboundAuthPolicy::OFF);
    fx.start(h);
    auto w = h.conn->written();
    assert(!HAS(w, "AUTH LOGIN PLAIN")); // auth_policy=OFF 不宣告 AUTH
    std::cout << "  [PASS] ehlo_with_auth_policy_off" << std::endl;
}

TEST(ehlo_no_starttls_when_disabled) {
    auto h = fx.make_session("EHLO test\r\n", InboundAuthPolicy::OFF, false);
    fx.start(h);
    auto w = h.conn->written();
    assert(HAS(w, "250-8BITMIME"));      // 其余能力照常通告
    assert(!HAS(w, "STARTTLS"));         // advertise_starttls=false 不宣告 STARTTLS
    std::cout << "  [PASS] ehlo_no_starttls_when_disabled" << std::endl;
}

TEST(starttls_rejected_when_disabled) {
    // 未通告 STARTTLS 的监听器收到 STARTTLS：502 拒绝，且会话保持可用继续收信
    auto h = fx.make_session(
        "EHLO test\r\n"
        "STARTTLS\r\n"
        "NOOP\r\n", InboundAuthPolicy::OFF, false);
    fx.start(h);
    assert(wait_for_response(h, "502 5.5.1 STARTTLS not supported"));
    assert(wait_for_response(h, "250 "));   // NOOP 正常应答，会话未被 STARTTLS 提前交接
    std::cout << "  [PASS] starttls_rejected_when_disabled" << std::endl;
}

TEST(mail_from_ok) {
    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<sender@client.example>\r\n");
    fx.start(h);
    auto w = h.conn->written();
    assert(HAS(w, "250 Ok"));
    std::cout << "  [PASS] mail_from_ok" << std::endl;
}

TEST(rcpt_to_ok) {
    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<sender@client.example>\r\n"
        "RCPT TO:<rcpt@test.local>\r\n");
    fx.start(h);
    auto w = h.conn->written();
    assert(HAS(w, "250 Ok"));
    std::cout << "  [PASS] rcpt_to_ok" << std::endl;
}

TEST(data_354) {
    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<sender@client.example>\r\n"
        "RCPT TO:<rcpt@test.local>\r\n"
        "DATA\r\n");
    fx.start(h);
    auto w = h.conn->written();
    assert(HAS(w, "354 Start mail input"));
    std::cout << "  [PASS] data_354" << std::endl;
}

// ========== RCPT 别名/邮件组（mail_aliases 展开，2026-10-18）==========

// 轮询等待收件人列表填充（认证路径的 RCPT 解析异步于喂入返回）
template <typename Handle>
static bool wait_for_recipient_count(Handle& h, size_t n, int timeout_ms = 2000) {
    auto* c = static_cast<SmtpsContext*>(h.session->get_context());
    for (int waited = 0; waited < timeout_ms; waited += 5) {
        h.conn->pump_executor();
        if (c->recipient_addresses.size() >= n) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    h.conn->pump_executor();
    return c->recipient_addresses.size() >= n;
}

TEST(rcpt_alias_expands_targets) {
    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<sender@client.example>\r\n"
        "RCPT TO:<team@test.local>\r\n");
    fx.start(h);
    assert(wait_for_response(h, "250 Ok"));
    auto* c = static_cast<SmtpsContext*>(h.session->get_context());
    // 别名本身不进收件人列表，展开为全部在用目标（邮件组语义）
    assert(c->recipient_addresses.size() == 2);
    assert(c->recipient_addresses[0] == "user1@test.local");
    assert(c->recipient_addresses[1] == "user2@test.local");
    std::cout << "  [PASS] rcpt_alias_expands_targets" << std::endl;
}

TEST(rcpt_alias_dedup_against_explicit_recipient) {
    // 同一封邮件：组展开目标与显式 RCPT 的真实用户重复时只投一次，
    // 重复记录会撞 mail_mailbox 唯一键回滚整个事务
    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<sender@client.example>\r\n"
        "RCPT TO:<team@test.local>\r\n"
        "RCPT TO:<user1@test.local>\r\n"
        "RCPT TO:<team@test.local>\r\n");
    fx.start(h);
    assert(wait_for_recipient_count(h, 2));
    auto* c = static_cast<SmtpsContext*>(h.session->get_context());
    assert(c->recipient_addresses.size() == 2);  // user1 去重，team 二次展开全跳过
    std::cout << "  [PASS] rcpt_alias_dedup_against_explicit_recipient" << std::endl;
}

TEST(rcpt_alias_unknown_still_550) {
    // 非用户非别名（负收件缓存 + 别名缓存 miss + 无 DB pool → 空目标）→ 550
    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<sender@client.example>\r\n"
        "RCPT TO:<nobody@test.local>\r\n");
    fx.start(h);
    assert(wait_for_response(h, "550 5.1.1 User unknown"));
    auto* c = static_cast<SmtpsContext*>(h.session->get_context());
    assert(c->recipient_addresses.empty());
    std::cout << "  [PASS] rcpt_alias_unknown_still_550" << std::endl;
}

TEST(rcpt_real_user_not_rerouted) {
    // 真实用户优先：正收件缓存命中直接收本人，别名缓存不参与
    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<sender@client.example>\r\n"
        "RCPT TO:<rcpt@test.local>\r\n");
    fx.start(h);
    assert(wait_for_response(h, "250 Ok"));
    auto* c = static_cast<SmtpsContext*>(h.session->get_context());
    assert(c->recipient_addresses.size() == 1);
    assert(c->recipient_addresses[0] == "rcpt@test.local");
    std::cout << "  [PASS] rcpt_real_user_not_rerouted" << std::endl;
}

TEST(auth_rcpt_alias_expands_targets) {
    // 587 提交路径同一展开语义：webmail/客户端给组发信必须达全体成员
    std::string plain;
    plain += '\0'; plain += "user0@test.local"; plain += '\0'; plain += "test123";
    std::string token = mail_system::outbound::base64_encode(
        reinterpret_cast<const unsigned char*>(plain.data()), plain.size());
    auto h = fx.make_session(
        "EHLO test\r\n"
        "AUTH PLAIN " + token + "\r\n"
        "MAIL FROM:<user0@test.local>\r\n"
        "RCPT TO:<team@test.local>\r\n",
        InboundAuthPolicy::ON);
    fx.start(h);
    assert(wait_for_response(h, "235 Authentication successful"));
    assert(wait_for_recipient_count(h, 2));
    auto* c = static_cast<SmtpsContext*>(h.session->get_context());
    assert(c->recipient_addresses[0] == "user1@test.local");
    assert(c->recipient_addresses[1] == "user2@test.local");
    std::cout << "  [PASS] auth_rcpt_alias_expands_targets" << std::endl;
}

TEST(auth_rcpt_nonalias_local_accepted_as_is) {
    // 提交模式不因别名查询新增拒绝理由：本地非别名收件人原样接受
    //（别名缓存 miss + 无 DB pool → 空目标 → 按原收件人入列表）
    std::string plain;
    plain += '\0'; plain += "user0@test.local"; plain += '\0'; plain += "test123";
    std::string token = mail_system::outbound::base64_encode(
        reinterpret_cast<const unsigned char*>(plain.data()), plain.size());
    auto h = fx.make_session(
        "EHLO test\r\n"
        "AUTH PLAIN " + token + "\r\n"
        "MAIL FROM:<user0@test.local>\r\n"
        "RCPT TO:<someone@test.local>\r\n",
        InboundAuthPolicy::ON);
    fx.start(h);
    assert(wait_for_response(h, "235 Authentication successful"));
    assert(wait_for_recipient_count(h, 1));
    auto* c = static_cast<SmtpsContext*>(h.session->get_context());
    assert(c->recipient_addresses[0] == "someone@test.local");
    assert(!HAS(h.conn->written(), "550"));
    std::cout << "  [PASS] auth_rcpt_nonalias_local_accepted_as_is" << std::endl;
}

TEST(full_delivery_pipeline) {
    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<sender@client.example>\r\n"
        "RCPT TO:<rcpt@test.local>\r\n"
        "DATA\r\n"
        "From: sender@client.example\r\n"
        "To: rcpt@test.local\r\n"
        "Subject: FSM test\r\n"
        "\r\n"
        "hello smtp fsm\r\n"
        ".\r\n"
        "QUIT\r\n");
    size_t mails_accepted_before = fx.server->mails_accepted_total_.load();
    fx.start(h);
    auto w = h.conn->written();
    assert(HAS(w, "220 SMTPS Server"));
    assert(HAS(w, "250-test.local Hello"));  // EHLO 响应首行=服务器域名
    assert(HAS(w, "250 Ok"));       // MAIL FROM + RCPT TO
    assert(HAS(w, "354 Start mail input"));
    assert(HAS(w, "250 "));         // message accepted (queue ID)
    assert(HAS(w, "221 Bye"));      // QUIT
    // 剧情 invariant：AFTER_ENQUEUE 模式下 finish_data_end_after_read 走 submit_owned_mail
    // → reset_mail_state → increment_mails_accepted。这是「FSM → 持久化」跨函数契约断言
    // (不是协议层断言)。mails_accepted_total_ 单调递增，无竞争。
    size_t mails_accepted_after = fx.server->mails_accepted_total_.load();
    if (mails_accepted_after <= mails_accepted_before) {
        std::cerr << "  [DEBUG] mails_accepted_before=" << mails_accepted_before
                  << " after=" << mails_accepted_after << std::endl;
    }
    assert(mails_accepted_after > mails_accepted_before &&
           "DATA END should have triggered submit_owned_mail + mails_accepted++");
    std::cout << "  [PASS] full_delivery_pipeline" << std::endl;
}

TEST(long_header_line_folded_on_ingest) {
    // 入站行折叠（2026-09-05 防御层）：>2048 字节的头行落盘前被折叠成
    // ≤2048 字节的合规行，投递流程不受影响；unfold 后语义可完整还原。
    std::string long_subject(5000, 'x');
    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<sender@client.example>\r\n"
        "RCPT TO:<rcpt@test.local>\r\n"
        "DATA\r\n"
        "From: sender@client.example\r\n"
        "To: rcpt@test.local\r\n"
        "Subject: " + long_subject + "\r\n"
        "\r\n"
        "body\r\n"
        ".\r\n"
        "QUIT\r\n");
    fx.start(h);
    auto w = h.conn->written();
    assert(HAS(w, "250 "));   // 折叠不阻断投递

    // 落盘的原始报文（mail/<id>）每一行都 ≤2048 字节；Subject 折叠块
    // unfold（删除 "\r\n "）后与原值逐字一致
    namespace fs = std::filesystem;
    const std::string want_subject = "Subject: " + long_subject;
    bool subject_verified = false;
    bool checked_raw_file = false;
    // 落盘在 enqueue 后由 worker 异步完成，单次扫描会撞上空窗 → 轮询至多 1s
    for (int pass = 0; pass < 200 && !subject_verified; ++pass) {
    h.conn->pump_executor();   // 落盘链路同样经 exec_ctx_,排空才推进
    for (auto& e : fs::directory_iterator(test_support::temp_path("smtps_fsm_test_mail"))) {
        if (!e.is_regular_file()) continue;
        if (e.path().extension() == ".mime") continue;   // 结构 JSON，非原始报文
        std::ifstream in(e.path(), std::ios::binary);
        if (!in.is_open()) continue;   // 异步持久化可能在扫描中途增删文件，跳过即可
        std::string line;
        std::string folded_subject;
        bool in_subject = false;
        auto verify_folded = [&]() {
            if (folded_subject.empty()) return;
            // unfold：删除我们插入的折叠序列
            std::string unfolded = folded_subject;
            const std::string fold_seq = "\r\n ";
            size_t pos;
            while ((pos = unfolded.find(fold_seq)) != std::string::npos)
                unfolded.erase(pos, fold_seq.size());
            if (unfolded == want_subject) subject_verified = true;
            folded_subject.clear();
        };
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            assert(line.size() <= LineFolder::kMaxStorageLineBytes);
            checked_raw_file = true;
            if (line.rfind("Subject: ", 0) == 0) {
                if (line == want_subject) { subject_verified = true; break; }  // 未折叠的短主题
                if (line.size() > LineFolder::kMaxStorageLineBytes - 16) {
                    folded_subject = line;                 // 长主题首行（被折叠的那个）
                    in_subject = true;
                }
            } else if (in_subject) {
                if (!line.empty() && line[0] == ' ') {
                    folded_subject += "\r\n" + line;       // 折叠续行
                } else {
                    verify_folded();                       // 折叠块结束（如头/体空行）
                    in_subject = false;
                }
            }
        }
        verify_folded();                                   // 文件结束时仍处于折叠块内
    }
    if (!subject_verified) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!checked_raw_file) {
        for (auto& e : fs::directory_iterator(test_support::temp_path("smtps_fsm_test_mail")))
            std::cerr << "  DIAG maildir entry: " << e.path().string() << std::endl;
        std::cerr << "  DIAG written tail: [" << h.conn->written().substr(
            h.conn->written().size() > 300 ? h.conn->written().size() - 300 : 0) << "]" << std::endl;
    }
    assert(checked_raw_file);
    assert(subject_verified);
    std::cout << "  [PASS] long_header_line_folded_on_ingest" << std::endl;
}

TEST(multiple_rcpt) {
    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<sender@client.example>\r\n"
        "RCPT TO:<rcpt1@test.local>\r\n"
        "RCPT TO:<rcpt2@test.local>\r\n"
        "RCPT TO:<rcpt3@test.local>\r\n"
        "QUIT\r\n");
    fx.start(h);
    auto w = h.conn->written();
    // 每个 RCPT TO 都返回 250 Ok
    size_t cnt = 0;
    size_t pos = 0;
    while ((pos = w.find("250 Ok", pos)) != std::string::npos) { ++cnt; ++pos; }
    assert(cnt >= 4); // MAIL FROM + 3x RCPT TO
    std::cout << "  [PASS] multiple_rcpt (250 Ok count=" << cnt << ")" << std::endl;
}

TEST(quit_221) {
    auto h = fx.make_session("QUIT\r\n");
    fx.start(h);
    auto w = h.conn->written();
    assert(HAS(w, "221 Bye"));
    std::cout << "  [PASS] quit_221" << std::endl;
}

TEST(re_ehlo_in_wait_auth) {
    // EHLO 后再次 EHLO — 应重置状态
    auto h = fx.make_session(
        "EHLO first\r\n"
        "EHLO second\r\n");
    fx.start(h);
    auto w = h.conn->written();
    assert(HAS(w, "250-test.local Hello"));  // EHLO 后再次 EHLO，首行仍=服务器域名
    assert(HAS(w, "250-test.local Hello"));
    std::cout << "  [PASS] re_ehlo_in_wait_auth" << std::endl;
}

// ========== Error Path ==========

TEST(invalid_command_sequence) {
    // 不发 EHLO 直接 MAIL FROM
    auto h = fx.make_session("MAIL FROM:<x@y>\r\n");
    fx.start(h);
    auto w = h.conn->written();
    assert(HAS(w, "500 Error"));
    std::cout << "  [PASS] invalid_command_sequence" << std::endl;
}

TEST(mail_from_bad_syntax) {
    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:bad-syntax\r\n");
    fx.start(h);
    auto w = h.conn->written();
    assert(HAS(w, "501 Syntax error"));
    std::cout << "  [PASS] mail_from_bad_syntax" << std::endl;
}

TEST(rcpt_to_bad_syntax) {
    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<sender@client.example>\r\n"
        "RCPT TO:bad\r\n");
    fx.start(h);
    auto w = h.conn->written();
    assert(HAS(w, "501 Syntax error"));
    std::cout << "  [PASS] rcpt_to_bad_syntax" << std::endl;
}

// ========== RCPT TO 收件人校验（防开放中继 + 防孤儿数据） ==========

TEST(rcpt_relay_denied) {
    // 未认证 MTA 路径：外部域名收件人 → 拒绝中继
    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<spam@attacker.com>\r\n"
        "RCPT TO:<victim@gmail.com>\r\n");
    fx.start(h);
    auto w = h.conn->written();
    assert(HAS(w, "550 5.7.1 Relay access denied"));
    std::cout << "  [PASS] rcpt_relay_denied" << std::endl;
}

// trusted_relay_networks 配置笔误（空前缀 / 非数字前缀 / 越界前缀 / 无斜杠垃圾）：
//   1) 不许炸：修复前 std::stoi("") 在 RCPT handler（io 线程）抛 invalid_argument，
//      本测试进程直接 abort；线上 io 线程的 run() 被 catch 后退出且不重进，挂在该
//      io_context 上的全部连接静默僵死（比崩溃更难发现——进程还活着）。
//   2) 不许误信任：坏前缀若被宽松解析成 0 = /0 = 匹配全网，等于对外开放中继。
//   3) 坏条目之后的好条目仍要生效（跳过而非中止遍历）。
TEST(rcpt_trusted_networks_malformed_entries_skipped) {
    auto count = [](const std::string& s, const std::string& sub) {
        size_t n = 0;
        for (size_t pos = 0; (pos = s.find(sub, pos)) != std::string::npos; pos += sub.size()) ++n;
        return n;
    };
    auto base = std::atomic_load(&fx.server->m_config);

    // 只有坏条目：外部收件人必须仍被拒 → 没有任何条目被误解析成信任
    {
        ServerConfig c = *base;
        c.trusted_relay_networks = {"10.0.0.0/", "192.168.0.0/x", "garbage", "10.0.0.0/99", "/8"};
        std::atomic_store(&fx.server->m_config, std::make_shared<ServerConfig>(c));

        auto h = fx.make_session(
            "EHLO test\r\n"
            "MAIL FROM:<spam@attacker.com>\r\n"
            "RCPT TO:<victim@gmail.com>\r\n");
        fx.start(h);
        auto w = h.conn->written();
        assert(HAS(w, "550 5.7.1 Relay access denied"));
    }

    // 坏条目后面跟一个覆盖 127.0.0.1（mock 客户端 IP）的好条目：必须放行 → 两个 250 Ok、无 550
    {
        ServerConfig c = *base;
        c.trusted_relay_networks = {"10.0.0.0/", "garbage", "127.0.0.0/8"};
        std::atomic_store(&fx.server->m_config, std::make_shared<ServerConfig>(c));

        auto h = fx.make_session(
            "EHLO test\r\n"
            "MAIL FROM:<spam@attacker.com>\r\n"
            "RCPT TO:<victim@gmail.com>\r\n");
        fx.start(h);
        auto w = h.conn->written();
        assert(!HAS(w, "550"));
        assert(count(w, "250 Ok") == 2);   // MAIL FROM + RCPT TO（信任中继放行）
    }

    std::atomic_store(&fx.server->m_config, base);   // 还原，后续用例共用 fixture
    std::cout << "  [PASS] rcpt_trusted_networks_malformed_entries_skipped" << std::endl;
}

TEST(rcpt_unknown_local_rejected) {
    // 未认证 MTA 路径：本地不存在用户 → 550 User unknown（负缓存）
    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<spam@attacker.com>\r\n"
        "RCPT TO:<nobody@test.local>\r\n");
    fx.start(h);
    auto w = h.conn->written();
    assert(HAS(w, "550 5.1.1 User unknown"));
    std::cout << "  [PASS] rcpt_unknown_local_rejected" << std::endl;
}

TEST(rcpt_local_known_accepted) {
    // 未认证 MTA 路径：本地已注册用户 → 250 Ok
    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<sender@client.example>\r\n"
        "RCPT TO:<rcpt@test.local>\r\n");
    fx.start(h);
    auto w = h.conn->written();
    assert(HAS(w, "250 Ok"));
    std::cout << "  [PASS] rcpt_local_known_accepted" << std::endl;
}

TEST(rcpt_auth_client_any_recipient) {
    // 认证客户端（465/587 提交）：允许任意收件人（含外部域名）
    auto h = fx.make_session(
        "EHLO test\r\n"
        "AUTH LOGIN\r\n"
        "dXNlcjBAdGVzdC5sb2NhbA==\r\n"     // base64("user0@test.local")
        "dGVzdDEyMw==\r\n"                   // base64("test123")
        "MAIL FROM:<user0@test.local>\r\n"
        "RCPT TO:<forward@example.com>\r\n",
        InboundAuthPolicy::ON);
    fx.start(h);
    assert(wait_for_response(h, "235 Authentication successful"));
    auto w = h.conn->written();
    assert(HAS(w, "250 Ok"));   // RCPT 接受外部域名
    std::cout << "  [PASS] rcpt_auth_client_any_recipient" << std::endl;
}

// ========== AUTH Flow ==========

TEST(auth_required_rejected) {
    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<sender@client.example>\r\n",
        InboundAuthPolicy::ON);
    fx.start(h);
    auto w = h.conn->written();
    assert(HAS(w, "530 5.7.1 Authentication required"));
    std::cout << "  [PASS] auth_required_rejected" << std::endl;
}

TEST(auth_login_username_prompt) {
    auto h = fx.make_session(
        "EHLO test\r\n"
        "AUTH LOGIN\r\n",
        InboundAuthPolicy::ON);
    fx.start(h);
    auto w = h.conn->written();
    assert(HAS(w, "334 VXNlcm5hbWU6")); // base64("Username:")
    std::cout << "  [PASS] auth_login_username_prompt" << std::endl;
}

TEST(auth_login_full_flow) {
    // AUTH LOGIN → username (base64) → password (base64) → success
    auto h = fx.make_session(
        "EHLO test\r\n"
        "AUTH LOGIN\r\n"
        "dXNlcjBAdGVzdC5sb2NhbA==\r\n"     // base64("user0@test.local")
        "dGVzdDEyMw==\r\n",                   // base64("test123")
        InboundAuthPolicy::ON);
    fx.start(h);
    // 流水线输入下 334 提示攒在 pending_write_buf_，随 worker 回复的最终
    // 写入一起冲出 —— 先等最终响应再断言中间提示
    assert(wait_for_response(h, "235 Authentication successful"));
    auto w = h.conn->written();
    assert(HAS(w, "334 VXNlcm5hbWU6"));       // Username prompt
    assert(HAS(w, "334 UGFzc3dvcmQ6"));       // Password prompt
    std::cout << "  [PASS] auth_login_full_flow" << std::endl;
}

TEST(auth_login_wrong_password) {
    auto h = fx.make_session(
        "EHLO test\r\n"
        "AUTH LOGIN\r\n"
        "dXNlcjBAdGVzdC5sb2NhbA==\r\n"     // base64("user0@test.local")
        "d3JvbmdwYXNz\r\n",                   // base64("wrongpass")
        InboundAuthPolicy::ON);
    fx.start(h);
    assert(wait_for_response(h, "535 Authentication failed"));
    auto w = h.conn->written();
    std::cout << "  [PASS] auth_login_wrong_password" << std::endl;
}

TEST(auth_failures_exceed_close) {
    // 3次密码错误 → 连接被关闭（不发535）
    auto h = fx.make_session(
        "EHLO test\r\n"
        "AUTH LOGIN\r\n"
        "dXNlcjBAdGVzdC5sb2NhbA==\r\n"     // user0@test.local
        "d3JvbmdwYXNz\r\n"                   // wrong pass - attempt 1
        "AUTH LOGIN\r\n"
        "dXNlcjBAdGVzdC5sb2NhbA==\r\n"
        "d3JvbmdwYXNz\r\n"                   // attempt 2
        "AUTH LOGIN\r\n"
        "dXNlcjBAdGVzdC5sb2NhbA==\r\n"
        "d3JvbmdwYXNz\r\n",                  // attempt 3 → close
        InboundAuthPolicy::ON);
    fx.start(h);

    // 异步认证：等 worker 线程的回复（任一 535 到达，或连接被关闭）
    size_t cnt = 0;
    std::string w;
    for (int waited = 0; waited < 2000; waited += 5) {
        w = h.conn->written();
        cnt = 0;
        for (size_t pos = 0; (pos = w.find("535 Authentication failed", pos)) != std::string::npos; ++pos) ++cnt;
        if (!h.conn->is_open() || cnt >= 1) break;
        h.conn->pump_executor();   // 535/关闭均经 exec_ctx_,排空后再判
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    h.conn->pump_executor();
    // 多次认证失败后连接应关闭
    assert(!h.conn->is_open() || cnt >= 1);

    std::cout << "  [PASS] auth_failures_exceed_close (535 count=" << cnt << ")" << std::endl;
}

TEST(auth_plain_single_step) {
    // AUTH PLAIN with inline token: \0user0@test.local\0test123
    std::string plain;
    plain += '\0'; plain += "user0@test.local"; plain += '\0'; plain += "test123";
    std::string token = mail_system::outbound::base64_encode(
        reinterpret_cast<const unsigned char*>(plain.data()), plain.size());
    auto h = fx.make_session(
        "EHLO test\r\n"
        "AUTH PLAIN " + token + "\r\n",
        InboundAuthPolicy::ON);
    fx.start(h);
    assert(wait_for_response(h, "235 Authentication successful"));
    auto w = h.conn->written();
    std::cout << "  [PASS] auth_plain_single_step" << std::endl;
}

TEST(auth_plain_two_step) {
    std::string plain2;
    plain2 += '\0'; plain2 += "user0@test.local"; plain2 += '\0'; plain2 += "test123";
    auto h = fx.make_session(
        "EHLO test\r\n"
        "AUTH PLAIN\r\n"
        + mail_system::outbound::base64_encode(
            reinterpret_cast<const unsigned char*>(plain2.data()), plain2.size()) + "\r\n",
        InboundAuthPolicy::ON);
    fx.start(h);
    assert(wait_for_response(h, "235 Authentication successful"));
    auto w = h.conn->written();
    assert(HAS(w, "334 "));                      // continue prompt
    std::cout << "  [PASS] auth_plain_two_step" << std::endl;
}

// ========== STARTTLS ==========

TEST(starttls) {
    std::string captured;
    auto conn_u = std::make_unique<MockConnection>();
    conn_u->capture_to(&captured);
    conn_u->set_read_data("EHLO test\r\nSTARTTLS\r\n");

    auto session = std::make_shared<SmtpsSession<MockConnection>>(
        fx.server.get(), std::move(conn_u),
        fx.fsm);

    ListenerConfig lc;
    lc.auth_policy = InboundAuthPolicy::OFF;
    session->set_listener_config(lc);
    session->set_current_state(static_cast<int>(SmtpsState::INIT));
    session->set_next_event(static_cast<int>(SmtpsEvent::CONNECT));
    session->process_read();

    // connection 已被 STARTTLS handler 释放，用 captured 断言
    assert(HAS(captured, "220 Ready to start TLS"));
    std::cout << "  [PASS] starttls" << std::endl;
}

// ========== 新增: 命令变体 ==========

TEST(helo_not_ehlo) {
    // HELO 应返回基本问候，不扩展 ESMTP 能力
    auto h = fx.make_session("HELO test.local\r\n");
    fx.start(h);
    auto w = h.conn->written();
    // HELO 响应格式验证
    assert(HAS(w, "250"));
    std::cout << "  [PASS] helo_not_ehlo" << std::endl;
}

TEST(noop_response) {
    // NOOP 在 wait_auth 状态下返回 250
    auto h = fx.make_session(
        "EHLO test\r\n"
        "NOOP\r\n"
        "QUIT\r\n");
    fx.start(h);
    auto w = h.conn->written();
    // NOOP 在 wait_auth 状态下返回响应
    assert(HAS(w, "250"));
    std::cout << "  [PASS] noop_response" << std::endl;
}

TEST(rset_envelope_reset) {
    // RSET 后可以重新开始 MAIL FROM
    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<s1@client.example>\r\n"
        "RCPT TO:<r1@test.local>\r\n"
        "RSET\r\n"
        "MAIL FROM:<s2@client.example>\r\n"
        "RCPT TO:<r2@test.local>\r\n"
        "QUIT\r\n");
    fx.start(h);
    auto w = h.conn->written();
    assert(HAS(w, "250"));  // RSET 响应或后续 MAIL FROM 的 250
    std::cout << "  [PASS] rset_envelope_reset" << std::endl;
}

// ========== 新增: 多事务 ==========

TEST(multiple_transactions) {
    // 一条连接中发送两封邮件
    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<s1@client.example>\r\n"
        "RCPT TO:<r1@test.local>\r\n"
        "DATA\r\n"
        "Subject: first\r\n"
        "\r\n"
        "body1\r\n"
        ".\r\n"
        "MAIL FROM:<s2@client.example>\r\n"
        "RCPT TO:<r2@test.local>\r\n"
        "DATA\r\n"
        "Subject: second\r\n"
        "\r\n"
        "body2\r\n"
        ".\r\n"
        "QUIT\r\n");
    size_t mails_accepted_before = fx.server->mails_accepted_total_.load();
    fx.start(h);
    auto w = h.conn->written();
    // 应该有两次 250 OK (排队 ID)
    size_t cnt = 0;
    size_t pos = 0;
    while ((pos = w.find("250 ", pos)) != std::string::npos) { ++cnt; ++pos; }
    assert(cnt >= 3); // greeting + 2 queue-ID 接受
    // 剧情 invariant：两封邮件 → 至少 2 次 submit_owned_mail → mails_accepted_total += 2
    size_t mails_accepted_after = fx.server->mails_accepted_total_.load();
    if (mails_accepted_after < mails_accepted_before + 2) {
        std::cerr << "  [DEBUG] mails_accepted before=" << mails_accepted_before
                  << " after=" << mails_accepted_after << std::endl;
    }
    assert(mails_accepted_after >= mails_accepted_before + 2 &&
           "Two DATA END should have triggered 2 submit_owned_mail -> mails_accepted += 2");
    std::cout << "  [PASS] multiple_transactions (250 OK count=" << cnt << ")" << std::endl;
}

// ========== 新增: DATA 边界条件 ==========

TEST(empty_body) {
    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<s@client.example>\r\n"
        "RCPT TO:<r@test.local>\r\n"
        "DATA\r\n"
        "\r\n"
        ".\r\n"
        "QUIT\r\n");
    fx.start(h);
    auto w = h.conn->written();
    assert(HAS(w, "250 "));  // 空 body 也接受
    std::cout << "  [PASS] empty_body" << std::endl;
}

TEST(dot_stuffing) {
    // 行首的 "." 会被去填充 (.. → .)
    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<s@client.example>\r\n"
        "RCPT TO:<r@test.local>\r\n"
        "DATA\r\n"
        "Subject: dot test\r\n"
        "\r\n"
        "..leading dot\r\n"
        "normal line\r\n"
        ".\r\n"
        "QUIT\r\n");
    fx.start(h);
    auto w = h.conn->written();
    assert(HAS(w, "250 "));  // dot-stuffing 处理正确
    std::cout << "  [PASS] dot_stuffing" << std::endl;
}

TEST(data_without_rcpt) {
    // 没有 RCPT TO 就发 DATA → 503 Bad sequence
    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<s@client.example>\r\n"
        "DATA\r\n");
    fx.start(h);
    auto w = h.conn->written();
    // 没有 RCPT TO 就发 DATA → 应返回错误
    assert(HAS(w, "50") || HAS(w, "503"));  // 503 Bad sequence
    std::cout << "  [PASS] data_without_rcpt" << std::endl;
}

// ========== 新增: Timeout/Error 事件 ==========

TEST(timeout_handler) {
    auto h = fx.make_session();
    fx.fsm->process_event(h.session, SmtpsEvent::TIMEOUT);
    h.conn->pump_executor();
    // The FSM's empty-input TIMEOUT event arms a read. Actual watchdog expiry
    // is a separate SessionBase timer path; lack of script data is not EOF.
    assert(h.conn->is_open());
    assert(h.conn->has_pending_read());
    assert(static_cast<SmtpsEvent>(h.session->get_next_event()) == SmtpsEvent::TIMEOUT);
    std::cout << "  [PASS] timeout_handler" << std::endl;
}

TEST(error_handler) {
    auto h = fx.make_session();
    // Protocol errors are tolerated three times, then close on the fourth.
    // Previously the mock's implicit EOF falsely made the first error terminal.
    for (int failure = 1; failure <= 4; ++failure) {
        h.conn->clear_written();
        fx.fsm->process_event(h.session, SmtpsEvent::ERROR);
        h.conn->pump_executor();
        assert(h.session->stay_times_ == failure);
        if (failure <= 3) {
            assert(h.conn->is_open());
            assert(HAS(h.conn->written(), "500 Error:"));
        } else {
            assert(!h.conn->is_open());
        }
    }
    std::cout << "  [PASS] error_handler" << std::endl;
}

// ========== 新增: EHLO 重置 AUTH 状态 ==========

TEST(ehlo_resets_auth_in_progress) {
    auto h = fx.make_session(
        "EHLO test\r\n"
        "AUTH LOGIN\r\n",
        InboundAuthPolicy::ON);
    fx.start(h);
    // AUTH 已开始，现在连接仍开着，追加 EHLO
    h.conn->append_read_data("EHLO reset\r\nQUIT\r\n");
    h.session->process_read();
    auto w = h.conn->written();
    // EHLO 应重置 AUTH 状态并返回 250 问候
    assert(HAS(w, "250"));
    std::cout << "  [PASS] ehlo_resets_auth_in_progress" << std::endl;
}

// ========== 本域发件人边界（防伪造，2026-10-20）==========
// 攻击模型：公网 25 直连（未认证且不在 trusted_relay_networks）的会话
//  (A) 信封 MAIL FROM 伪造本域 —— 规则 1 在 MAIL FROM 550；
//  (B) 信封用自己域、只伪造 From: 头为本域 —— 规则 2 在 DATA_END 550 且不入队。
// 合法本域邮件（认证提交 / trusted relay）必须不受影响（规则/开关回归见下）。

TEST(local_sender_mail_from_rejected) {
    // 规则 1：未认证非受信会话，本域信封发件人 → 550，状态回 WAIT_AUTH 可继续
    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<user@test.local>\r\n"
        "MAIL FROM:<sender@client.example>\r\n"
        "MAIL FROM:<>\r\n");   // 空信封（退信路径）豁免
    fx.start(h);
    assert(wait_for_response(h, "550 5.7.1 Sender address rejected: local domain requires authentication"));
    // 状态回 WAIT_AUTH：外部域 MAIL FROM 正常 250，会话未被关闭
    assert(wait_for_response(h, "250 Ok"));
    assert(wait_for_response(h, "250 Ok"));   // <> 退信豁免
    std::cout << "  [PASS] local_sender_mail_from_rejected (550 + 状态恢复 + 空信封豁免)" << std::endl;
}

TEST(local_sender_trusted_relay_exempt) {
    // trusted_relay_networks 命中客户端 IP（mock=127.0.0.1）→ 本域信封发件人放行
    // （边缘 MTA 收主站出站邮件的豁免路径）
    auto base = std::atomic_load(&fx.server->m_config);
    ServerConfig c = *base;
    c.trusted_relay_networks = {"127.0.0.0/8"};
    std::atomic_store(&fx.server->m_config, std::make_shared<ServerConfig>(c));

    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<user@test.local>\r\n"
        "RCPT TO:<rcpt@test.local>\r\n");
    fx.start(h);
    assert(wait_for_response(h, "250 Ok"));

    std::atomic_store(&fx.server->m_config, base);   // 还原，后续用例共用 fixture
    std::cout << "  [PASS] local_sender_trusted_relay_exempt" << std::endl;
}

TEST(local_from_header_rejected_at_data_end) {
    // 规则 2：信封他域绕过 SPF，仅伪造 From: 头为本域 → DATA_END 550 且不入队
    size_t mails_accepted_before = fx.server->mails_accepted_total_.load();
    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<sender@client.example>\r\n"
        "RCPT TO:<rcpt@test.local>\r\n"
        "DATA\r\n"
        "From: anyone@test.local\r\n"
        "To: rcpt@test.local\r\n"
        "Subject: spoof\r\n"
        "\r\n"
        "body\r\n"
        ".\r\n");
    fx.start(h);
    assert(wait_for_response(h, "550 5.7.1 From header rejected: local domain requires authentication"));
    // 未入队（mails_accepted 不增）
    assert(fx.server->mails_accepted_total_.load() == mails_accepted_before);
    std::cout << "  [PASS] local_from_header_rejected_at_data_end (550 + 未入队)" << std::endl;
}

TEST(auth_from_mismatch_rejected_at_data_end) {
    // 规则 3：认证提交的 From: 头与认证账号不一致 → 550 丢弃
    std::string plain;
    plain += '\0'; plain += "user0@test.local"; plain += '\0'; plain += "test123";
    std::string token = mail_system::outbound::base64_encode(
        reinterpret_cast<const unsigned char*>(plain.data()), plain.size());
    size_t mails_accepted_before = fx.server->mails_accepted_total_.load();
    auto h = fx.make_session(
        "EHLO test\r\n"
        "AUTH PLAIN " + token + "\r\n"
        "MAIL FROM:<user0@test.local>\r\n"
        "RCPT TO:<rcpt@test.local>\r\n"
        "DATA\r\n"
        "From: colleague@test.local\r\n"
        "To: rcpt@test.local\r\n"
        "Subject: spoof\r\n"
        "\r\n"
        "body\r\n"
        ".\r\n",
        InboundAuthPolicy::ON);
    fx.start(h);
    assert(wait_for_response(h, "235 Authentication successful"));
    assert(wait_for_response(h, "550 5.7.1 From header does not match authenticated user"));
    assert(fx.server->mails_accepted_total_.load() == mails_accepted_before);
    std::cout << "  [PASS] auth_from_mismatch_rejected_at_data_end (550 + 未入队)" << std::endl;
}

TEST(auth_from_match_accepted_at_data_end) {
    // 规则 3 不误伤：From: 头等于认证账号 → 正常入队
    std::string plain;
    plain += '\0'; plain += "user0@test.local"; plain += '\0'; plain += "test123";
    std::string token = mail_system::outbound::base64_encode(
        reinterpret_cast<const unsigned char*>(plain.data()), plain.size());
    size_t mails_accepted_before = fx.server->mails_accepted_total_.load();
    auto h = fx.make_session(
        "EHLO test\r\n"
        "AUTH PLAIN " + token + "\r\n"
        "MAIL FROM:<user0@test.local>\r\n"
        "RCPT TO:<rcpt@test.local>\r\n"
        "DATA\r\n"
        "From: User0 <user0@test.local>\r\n"   // 显示名 + 大小写：归一后精确匹配
        "To: rcpt@test.local\r\n"
        "Subject: legit\r\n"
        "\r\n"
        "body\r\n"
        ".\r\n",
        InboundAuthPolicy::ON);
    fx.start(h);
    assert(wait_for_response(h, "235 Authentication successful"));
    assert(wait_for_response(h, "250 OK"));
    assert(fx.server->mails_accepted_total_.load() == mails_accepted_before + 1);
    std::cout << "  [PASS] auth_from_match_accepted_at_data_end (250 + 入队)" << std::endl;
}

TEST(policy_toggle_restores_old_behavior) {
    // 两个开关置 false → 恢复旧行为：未认证非受信的本域信封/From 头均照旧入队
    auto base = std::atomic_load(&fx.server->m_config);
    ServerConfig c = *base;
    c.reject_unauthenticated_local_sender = false;
    c.enforce_authenticated_from_match = false;
    std::atomic_store(&fx.server->m_config, std::make_shared<ServerConfig>(c));

    // 规则 1 关：本域信封发件人放行
    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<user@test.local>\r\n"
        "RCPT TO:<rcpt@test.local>\r\n");
    fx.start(h);
    assert(wait_for_response(h, "250 Ok"));

    // 规则 2 关：本域 From: 头照旧入队
    size_t mails_accepted_before = fx.server->mails_accepted_total_.load();
    auto h2 = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<sender@client.example>\r\n"
        "RCPT TO:<rcpt@test.local>\r\n"
        "DATA\r\n"
        "From: anyone@test.local\r\n"
        "To: rcpt@test.local\r\n"
        "Subject: legacy\r\n"
        "\r\n"
        "body\r\n"
        ".\r\n");
    fx.start(h2);
    assert(wait_for_response(h2, "250 OK"));
    assert(fx.server->mails_accepted_total_.load() == mails_accepted_before + 1);

    std::atomic_store(&fx.server->m_config, base);   // 还原，后续用例共用 fixture
    std::cout << "  [PASS] policy_toggle_restores_old_behavior" << std::endl;
}

TEST(auth_from_alias_accepted_at_data_end) {
    // 规则 3 别名分支：From 头地址是登录账号的别名（solo@test.local → user0）
    // → 视为本人发信，正常入队。信封 MAIL FROM 仍强制登录账号（不变）。
    std::string plain;
    plain += '\0'; plain += "user0@test.local"; plain += '\0'; plain += "test123";
    std::string token = mail_system::outbound::base64_encode(
        reinterpret_cast<const unsigned char*>(plain.data()), plain.size());
    size_t mails_accepted_before = fx.server->mails_accepted_total_.load();
    auto h = fx.make_session(
        "EHLO test\r\n"
        "AUTH PLAIN " + token + "\r\n"
        "MAIL FROM:<user0@test.local>\r\n"
        "RCPT TO:<rcpt@test.local>\r\n"
        "DATA\r\n"
        "From: Solo <solo@test.local>\r\n"
        "To: rcpt@test.local\r\n"
        "Subject: via alias\r\n"
        "\r\n"
        "body\r\n"
        ".\r\n",
        InboundAuthPolicy::ON);
    fx.start(h);
    assert(wait_for_response(h, "235 Authentication successful"));
    assert(wait_for_response(h, "250 OK"));
    assert(fx.server->mails_accepted_total_.load() == mails_accepted_before + 1);
    std::cout << "  [PASS] auth_from_alias_accepted_at_data_end (别名命中 → 入队)" << std::endl;
}

TEST(account_policy_completions_resume_on_connection_executor) {
    struct DeferredAuthenticator final : IAccountAuthenticator {
        Complete pending;
        void login_async(const std::string&, const std::string&, Complete complete, bool) override {
            pending = std::move(complete);
        }
    };
    auto authenticator = std::make_shared<DeferredAuthenticator>();
    fx.fsm->set_authenticator(authenticator);
    auto executor = std::make_shared<test::ManualExecutor>();
    auto connection = std::make_unique<MockConnection>(executor);
    auto session = std::make_shared<SmtpsSession<MockConnection>>(
        fx.server.get(), std::move(connection), fx.fsm);
    int completed = 0;
    fx.fsm->auth_user_async(session, "user0@test.local", "test123",
        [&](bool ok, int shard) {
            assert(executor->running); // A service/DB thread cannot reenter the protocol.
            assert(ok && shard == 3);
            ++completed;
        });
    assert(session->is_paused() && authenticator->pending && completed == 0);
    AccountAuthResult result;
    result.ok = true;
    result.shard = 3;
    auto complete = std::move(authenticator->pending);
    complete(std::move(result)); // Simulate a service/DB completion outside the executor.
    assert(completed == 0 && executor->tasks.size() == 1);
    executor->drain();
    assert(completed == 1);

    int policies = 0;
    fx.fsm->user_exists_async(session, "user0@test.local", [&](bool exists) {
        assert(executor->running && exists);
        ++policies;
    });
    fx.fsm->alias_targets_async(session, "solo@test.local", [&](std::vector<std::string> targets) {
        assert(executor->running && targets == std::vector<std::string>{"user0@test.local"});
        ++policies;
    });
    fx.fsm->check_send_quota_async(session, "user0@test.local", [&](bool allowed) {
        assert(executor->running && allowed); // Disabled quota still uses the same boundary.
        ++policies;
    });
    assert(policies == 0 && executor->tasks.size() == 3);
    executor->drain();
    assert(policies == 3);

    // Existing no-session fallback semantics remain available to non-session callers.
    fx.fsm->user_exists_async({}, "user0@test.local", [&](bool exists) { assert(!exists); ++policies; });
    fx.fsm->alias_targets_async({}, "solo@test.local", [&](std::vector<std::string> targets) {
        assert(targets == std::vector<std::string>{"user0@test.local"}); ++policies;
    });
    fx.fsm->check_send_quota_async({}, "user0@test.local", [&](bool allowed) { assert(allowed); ++policies; });
    assert(policies == 6);

    int after_close = 0;
    fx.fsm->user_exists_async(session, "user0@test.local", [&](bool) { ++after_close; });
    fx.fsm->alias_targets_async(session, "solo@test.local", [&](std::vector<std::string>) { ++after_close; });
    fx.fsm->check_send_quota_async(session, "user0@test.local", [&](bool) { ++after_close; });
    assert(executor->tasks.size() == 3);
    session->close();
    executor->drain();
    assert(after_close == 0);
    session.reset();
    fx.fsm->set_authenticator(std::make_shared<AccountAuthenticationService>(
        fx.router, fx.fsm->m_authCache, fx.worker_pool));
    std::cout << "  [PASS] account_policy_completions_resume_on_connection_executor" << std::endl;
}

TEST(delayed_auth_and_quota_resume_buffered_pipeline) {
    struct DeferredAuthenticator final : IAccountAuthenticator {
        Complete pending;
        void login_async(const std::string&, const std::string&, Complete complete, bool) override {
            pending = std::move(complete);
        }
    };
    auto authenticator = std::make_shared<DeferredAuthenticator>();
    fx.fsm->set_authenticator(authenticator);
    std::string plain;
    plain += '\0'; plain += "user0@test.local"; plain += '\0'; plain += "test123";
    const auto token = outbound::base64_encode(
        reinterpret_cast<const unsigned char*>(plain.data()), plain.size());
    auto h = fx.make_session(
        "EHLO test\r\nAUTH PLAIN " + token + "\r\n"
        "MAIL FROM:<user0@test.local>\r\n"
        "RCPT TO:<rcpt@test.local>\r\n"
        "NOOP\r\n", InboundAuthPolicy::ON);
    fx.start(h);
    assert(authenticator->pending && h.session->is_paused() && h.session->has_buffered_input());
    assert(!HAS(h.conn->written(), "235 Authentication successful"));

    AccountAuthResult result;
    result.ok = true;
    auto complete = std::move(authenticator->pending);
    complete(std::move(result));
    // Draining executes the deferred auth result, then the separately posted
    // disabled-quota completion. No new input or worker timing can drive RCPT.
    h.conn->pump_executor();
    const auto reply = h.conn->written();
    assert(HAS(reply, "235 Authentication successful"));
    assert(HAS(reply, "250 Ok"));
    // NOOP is currently unsupported by this parser. Its explicit response proves
    // the command after RCPT was consumed, independently of earlier 250 replies.
    assert(HAS(reply, "500 Error: Unknown command: NOOP"));
    auto* context = static_cast<SmtpsContext*>(h.session->get_context());
    assert(context->sender_address == "user0@test.local");
    assert(context->recipient_addresses == std::vector<std::string>{"rcpt@test.local"});
    assert(!h.session->is_paused() && !h.session->has_buffered_input());
    assert(!h.session->is_closed() && h.conn->has_pending_read());
    fx.fsm->set_authenticator(std::make_shared<AccountAuthenticationService>(
        fx.router, fx.fsm->m_authCache, fx.worker_pool));
    std::cout << "  [PASS] delayed_auth_and_quota_resume_buffered_pipeline" << std::endl;
}

TEST(idle_peer_waits_for_delayed_auth_then_explicit_eof_closes) {
    struct DeferredAuthenticator final : IAccountAuthenticator {
        Complete pending;
        void login_async(const std::string&, const std::string&, Complete complete, bool) override {
            pending = std::move(complete);
        }
    };
    auto authenticator = std::make_shared<DeferredAuthenticator>();
    fx.fsm->set_authenticator(authenticator);
    std::string plain;
    plain += '\0'; plain += "user0@test.local"; plain += '\0'; plain += "test123";
    const auto token = outbound::base64_encode(
        reinterpret_cast<const unsigned char*>(plain.data()), plain.size());
    auto h = fx.make_session("EHLO test\r\nAUTH PLAIN " + token + "\r\n", InboundAuthPolicy::ON);
    fx.start(h);
    assert(authenticator->pending && h.session->is_paused());
    assert(!h.session->is_closed() && h.conn->is_open());
    assert(!HAS(h.conn->written(), "235 Authentication successful"));

    AccountAuthResult result;
    result.ok = true;
    auto complete = std::move(authenticator->pending);
    complete(std::move(result)); // Delayed service completion after all scripted bytes were consumed.
    assert(wait_for_response(h, "235 Authentication successful"));
    assert(!h.session->is_closed() && h.conn->has_pending_read());

    // The peer can send another batch while still connected.
    h.conn->clear_written();
    h.conn->executor()->post([connection = h.conn] { connection->trigger_deferred_read("MAIL FROM:<user0@test.local>\r\n"); });
    h.conn->pump_executor();
    assert(HAS(h.conn->written(), "250"));
    assert(!h.session->is_closed() && h.conn->has_pending_read());

    // A real EOF remains terminal; the fixture does not hide disconnect handling.
    h.conn->executor()->post([connection = h.conn] { connection->trigger_deferred_read(""); });
    h.conn->pump_executor();
    assert(h.session->is_closed() && !h.conn->is_open());
    fx.fsm->set_authenticator(std::make_shared<AccountAuthenticationService>(
        fx.router, fx.fsm->m_authCache, fx.worker_pool));
    std::cout << "  [PASS] idle_peer_waits_for_delayed_auth_then_explicit_eof_closes" << std::endl;
}

TEST(auth_from_alias_case_normalized_accepted_at_data_end) {
    // 规则 3 别名分支大小写规范化：别名目标大小写与登录账号不同也命中
    fx.fsm->m_aliasCache->put("casealias@test.local", {"User0@Test.LOCAL"});
    std::string plain;
    plain += '\0'; plain += "user0@test.local"; plain += '\0'; plain += "test123";
    std::string token = mail_system::outbound::base64_encode(
        reinterpret_cast<const unsigned char*>(plain.data()), plain.size());
    auto h = fx.make_session(
        "EHLO test\r\n"
        "AUTH PLAIN " + token + "\r\n"
        "MAIL FROM:<user0@test.local>\r\n"
        "RCPT TO:<rcpt@test.local>\r\n"
        "DATA\r\n"
        "From: casealias@test.local\r\n"
        "To: rcpt@test.local\r\n"
        "Subject: case\r\n"
        "\r\n"
        "body\r\n"
        ".\r\n",
        InboundAuthPolicy::ON);
    fx.start(h);
    assert(wait_for_response(h, "235 Authentication successful"));
    assert(wait_for_response(h, "250 OK"));
    std::cout << "  [PASS] auth_from_alias_case_normalized_accepted_at_data_end" << std::endl;
}

TEST(auth_from_alias_miss_rejected_at_data_end) {
    // 规则 3 别名分支：From 地址是别名但不指向登录账号（team → user1,user2）→ 550
    std::string plain;
    plain += '\0'; plain += "user0@test.local"; plain += '\0'; plain += "test123";
    std::string token = mail_system::outbound::base64_encode(
        reinterpret_cast<const unsigned char*>(plain.data()), plain.size());
    size_t mails_accepted_before = fx.server->mails_accepted_total_.load();
    auto h = fx.make_session(
        "EHLO test\r\n"
        "AUTH PLAIN " + token + "\r\n"
        "MAIL FROM:<user0@test.local>\r\n"
        "RCPT TO:<rcpt@test.local>\r\n"
        "DATA\r\n"
        "From: team@test.local\r\n"
        "To: rcpt@test.local\r\n"
        "Subject: spoof via alias\r\n"
        "\r\n"
        "body\r\n"
        ".\r\n",
        InboundAuthPolicy::ON);
    fx.start(h);
    assert(wait_for_response(h, "235 Authentication successful"));
    assert(wait_for_response(h, "550 5.7.1 From header does not match authenticated user"));
    assert(fx.server->mails_accepted_total_.load() == mails_accepted_before);
    std::cout << "  [PASS] auth_from_alias_miss_rejected_at_data_end (别名未命中 → 550 未入队)" << std::endl;
}

TEST(auth_from_alias_db_unavailable_fails_closed_at_data_end) {
    // 规则 3 别名分支 fail-closed：别名缓存 miss + 无 DB pool（fixture 无 DB）→
    // 空目标视为不匹配 → 550（DB/缓存不可用不能变成绕过 From 校验的通道）
    std::string plain;
    plain += '\0'; plain += "user0@test.local"; plain += '\0'; plain += "test123";
    std::string token = mail_system::outbound::base64_encode(
        reinterpret_cast<const unsigned char*>(plain.data()), plain.size());
    size_t mails_accepted_before = fx.server->mails_accepted_total_.load();
    auto h = fx.make_session(
        "EHLO test\r\n"
        "AUTH PLAIN " + token + "\r\n"
        "MAIL FROM:<user0@test.local>\r\n"
        "RCPT TO:<rcpt@test.local>\r\n"
        "DATA\r\n"
        "From: ghostalias@test.local\r\n"
        "To: rcpt@test.local\r\n"
        "Subject: db down\r\n"
        "\r\n"
        "body\r\n"
        ".\r\n",
        InboundAuthPolicy::ON);
    fx.start(h);
    assert(wait_for_response(h, "235 Authentication successful"));
    assert(wait_for_response(h, "550 5.7.1 From header does not match authenticated user"));
    assert(fx.server->mails_accepted_total_.load() == mails_accepted_before);
    std::cout << "  [PASS] auth_from_alias_db_unavailable_fails_closed_at_data_end (空目标 → 550)" << std::endl;
}

TEST(data_missing_layout_fails_commit_without_storage_io) {
    auto original = fx.server->m_shardRouter;
    auto generic = std::make_shared<test::NoMailLayoutStorage>();
    fx.server->m_shardRouter = std::make_shared<router::StaticShardRouter>(
        std::vector<std::pair<std::string,int>>{}, 0, std::vector<std::shared_ptr<pr::DBPool>>{},
        std::vector<std::shared_ptr<pr::storage::IStorageProvider>>{generic});
    auto h = fx.make_session();
    h.session->create_mail_on_data_command();
    assert(h.session->get_mail() && h.session->get_mail()->body_path.empty() &&
           h.session->get_mail()->persist_status == pq::PersistStatus::CANCELLED && !generic->io_calls);
    h.session->set_current_state(static_cast<int>(SmtpsState::IN_MESSAGE));
    fx.fsm->process_event(h.session, SmtpsEvent::DATA_END);
    h.conn->pump_executor();
    assert(HAS(h.conn->written(), "550 5.3.0 Failed to store message") && !HAS(h.conn->written(), "250"));
    assert(!h.session->is_paused() && !generic->io_calls && !h.session->get_mail());
    fx.server->m_shardRouter = original;
    std::cout << "  [PASS] missing mail layout rejects DATA commit without guessing a path" << std::endl;
}

int main() {
    std::cout << "Inbound SMTP FSM Test Suite\n===========================\n";

    try {
        FsmTestFixture fx;

        // ── Happy Path ──
        test_greeting_220(fx);
        test_ehlo_capabilities(fx);
        test_ehlo_with_auth_policy_on(fx);
        test_ehlo_with_auth_policy_off(fx);
        test_mail_from_ok(fx);
        test_rcpt_to_ok(fx);
        test_data_354(fx);
        test_data_missing_layout_fails_commit_without_storage_io(fx);
        test_full_delivery_pipeline(fx);
        test_multiple_rcpt(fx);
        test_quit_221(fx);
        test_re_ehlo_in_wait_auth(fx);

        // ── Error Path ──
        test_invalid_command_sequence(fx);
        test_mail_from_bad_syntax(fx);
        test_rcpt_to_bad_syntax(fx);
        test_rcpt_relay_denied(fx);
        test_rcpt_trusted_networks_malformed_entries_skipped(fx);
        test_rcpt_unknown_local_rejected(fx);
        test_rcpt_local_known_accepted(fx);
        test_rcpt_auth_client_any_recipient(fx);

        // ── AUTH Flow ──
        test_auth_required_rejected(fx);
        test_auth_login_username_prompt(fx);
        test_auth_login_full_flow(fx);
        test_auth_login_wrong_password(fx);
        test_auth_failures_exceed_close(fx);
        test_auth_plain_single_step(fx);
        test_auth_plain_two_step(fx);

        // ── STARTTLS (已知: 重构后需 STARTTLS handler 适配) ──
        // test_starttls(fx);
        // advertise_starttls=false：25 端口明文投递开关（跨境 RST 规避，2026-09-28）
        test_ehlo_no_starttls_when_disabled(fx);
        test_starttls_rejected_when_disabled(fx);

        // ── 命令变体 ──
        test_helo_not_ehlo(fx);
        test_noop_response(fx);
        test_rset_envelope_reset(fx);

        // ── RCPT 别名/邮件组 ──
        test_rcpt_alias_expands_targets(fx);
        test_rcpt_alias_dedup_against_explicit_recipient(fx);
        test_rcpt_alias_unknown_still_550(fx);
        test_rcpt_real_user_not_rerouted(fx);
        test_auth_rcpt_alias_expands_targets(fx);
        test_auth_rcpt_nonalias_local_accepted_as_is(fx);

        // ── 多事务 ──
        test_multiple_transactions(fx);   // 串两封邮件入队

        // ── DATA 边界条件 ──
        test_empty_body(fx);
        test_dot_stuffing(fx);
        test_data_without_rcpt(fx);
        test_long_header_line_folded_on_ingest(fx);

        // ── Timeout/Error ──
        test_timeout_handler(fx);
        test_error_handler(fx);

        // ── EHLO 重置 AUTH ──
        test_ehlo_resets_auth_in_progress(fx);

        // ── 本域发件人边界（防伪造） ──
        test_local_sender_mail_from_rejected(fx);
        test_local_sender_trusted_relay_exempt(fx);
        test_local_from_header_rejected_at_data_end(fx);
        test_auth_from_mismatch_rejected_at_data_end(fx);
        test_auth_from_match_accepted_at_data_end(fx);
        test_auth_from_alias_accepted_at_data_end(fx);
        test_account_policy_completions_resume_on_connection_executor(fx);
        test_delayed_auth_and_quota_resume_buffered_pipeline(fx);
        test_idle_peer_waits_for_delayed_auth_then_explicit_eof_closes(fx);
        test_auth_from_alias_case_normalized_accepted_at_data_end(fx);
        test_auth_from_alias_miss_rejected_at_data_end(fx);
        test_auth_from_alias_db_unavailable_fails_closed_at_data_end(fx);
        test_policy_toggle_restores_old_behavior(fx);

        std::cout << "\nAll tests passed.\n";
    } catch (const std::exception& e) {
        std::cerr << "FATAL: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
