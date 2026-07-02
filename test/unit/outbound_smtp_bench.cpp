// Outbound SMTP FSM unit test — mock 远端服务器响应，验证状态转换和错误处理
// 使用 MockOutboundStream 零 I/O 测试 execute_smtp_transaction
#include "mail_system/back/outbound/smtp_outbound_client.h"
#include "mail_system/back/outbound/smtp_outbound_transaction.h"
#include "mock_outbound_stream.h"

#include <boost/asio.hpp>
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace mail_system;
using namespace mail_system::outbound;
using mail_system::test::MockOutboundStream;

namespace {

// 辅助：跑一次事务，返回结果
SmtpExecResult run_test(const std::vector<std::string>& responses,
                         bool expect_greeting = true,
                         bool allow_starttls = false) {
    MockOutboundStream stream;
    stream.set_responses(responses);
    boost::asio::streambuf buffer;

    OutboxRecord record;
    record.id = 1;
    record.mail_id = 100;
    record.sender = "sender@test.com";
    record.recipient = "rcpt@example.com";

    OutboundConfig id_cfg;
    id_cfg.helo_domain = "outbound.test";
    id_cfg.dkim_enabled = false;

    return execute_smtp_transaction(stream, buffer, record, nullptr,
        id_cfg.helo_domain, record.sender, record.sender, id_cfg,
        allow_starttls, expect_greeting, []() { return true; });
}

#define TEST(name) void test_##name(); struct register_##name { register_##name() { test_##name(); } } _r_##name; void test_##name()

// ========== happy path ==========

TEST(normal_delivery) {
    auto r = run_test({
        "220 mx.example.com ESMTP",  // greeting
        "250-STARTTLS\n250 SMTPUTF8", // EHLO
        "250 Ok",                     // MAIL FROM
        "250 Ok",                     // RCPT TO
        "354 Start mail input",       // DATA
        "250 OK id=abc123",           // body ack
    });
    assert(r.success);
    assert(!r.permanent_failure);
    std::cout << "  [PASS] normal_delivery: " << r.response << std::endl;
}

TEST(normal_delivery_no_greeting) {
    auto r = run_test({
        "250-STARTTLS\n250 SMTPUTF8",
        "250 Ok", "250 Ok", "354 Start mail input", "250 OK"
    }, false, false);
    assert(r.success);
    std::cout << "  [PASS] normal_delivery_no_greeting" << std::endl;
}

TEST(ehlo_fallback_to_helo) {
    auto r = run_test({
        "220 mx ESMTP",
        "500 Unknown EHLO",           // EHLO rejected
        "250 OK",                     // HELO accepted
        "250 Ok", "250 Ok", "354 Start", "250 OK"
    });
    assert(r.success);
    std::cout << "  [PASS] ehlo_fallback_to_helo" << std::endl;
}

TEST(starttls_upgrade) {
    auto r = run_test({
        "220 mx ESMTP",
        "250-STARTTLS\n250 SMTPUTF8",
        "220 Ready for TLS"           // STARTTLS accepted
    }, true, true);
    assert(!r.success); // returns with kStartTlsReadyToken
    assert(r.error_message == kStartTlsReadyToken);
    std::cout << "  [PASS] starttls_upgrade: " << r.error_message << std::endl;
}

// ========== error paths ==========

TEST(bad_greeting_4xx) {
    auto r = run_test({"421 Service unavailable"});
    assert(!r.success);
    assert(!r.permanent_failure); // 421 is 4xx, temporary
    std::cout << "  [PASS] bad_greeting (4xx temp): " << r.error_message << std::endl;
}

TEST(bad_greeting_5xx) {
    auto r = run_test({"550 Permanent failure"});
    assert(!r.success);
    assert(r.permanent_failure);
    std::cout << "  [PASS] bad_greeting (5xx perm): " << r.error_message << std::endl;
}

TEST(greeting_4xx_temp_fail) {
    auto r = run_test({"450 Mailbox unavailable"});
    assert(!r.success);
    assert(!r.permanent_failure); // 4xx is temporary
    std::cout << "  [PASS] greeting_4xx: " << r.error_message << std::endl;
}

TEST(mail_from_permanent_reject) {
    auto r = run_test({
        "220 mx ESMTP", "250 OK",     // greeting + EHLO
        "550 No such user"            // MAIL FROM rejected
    });
    assert(!r.success);
    assert(r.permanent_failure);
    std::cout << "  [PASS] mail_from_perm_reject: " << r.error_message << std::endl;
}

TEST(rcpt_to_temporary_reject) {
    auto r = run_test({
        "220 mx ESMTP", "250 OK",
        "250 Ok",                     // MAIL FROM OK
        "451 Try later"               // RCPT TO temp fail
    });
    assert(!r.success);
    assert(!r.permanent_failure);
    std::cout << "  [PASS] rcpt_to_temp_reject: " << r.error_message << std::endl;
}

TEST(data_rejected) {
    auto r = run_test({
        "220 mx ESMTP", "250 OK",
        "250 Ok", "250 Ok",
        "550 No permission"           // DATA rejected
    });
    assert(!r.success);
    assert(r.permanent_failure);
    std::cout << "  [PASS] data_rejected: " << r.error_message << std::endl;
}

TEST(body_rejected) {
    auto r = run_test({
        "220 mx ESMTP", "250 OK",
        "250 Ok", "250 Ok",
        "354 Start",
        "552 Message size exceeds"    // body rejected
    });
    assert(!r.success);
    assert(r.permanent_failure);
    std::cout << "  [PASS] body_rejected: " << r.error_message << std::endl;
}

TEST(body_temp_reject) {
    auto r = run_test({
        "220 mx ESMTP", "250 OK",
        "250 Ok", "250 Ok",
        "354 Start",
        "452 Insufficient storage"    // temp body reject
    });
    assert(!r.success);
    assert(!r.permanent_failure);
    std::cout << "  [PASS] body_temp_reject: " << r.error_message << std::endl;
}

TEST(data_250_realign) {
    // Some servers send 250 before 354 (realign)
    auto r = run_test({
        "220 mx ESMTP", "250 OK",
        "250 Ok", "250 Ok",           // MAIL + RCPT
        "250 Ok",                     // DATA gets 250 first
        "354 Start mail input",       // then 354
        "250 OK"
    });
    assert(r.success);
    std::cout << "  [PASS] data_250_realign" << std::endl;
}

TEST(mail_from_realign) {
    // EHLO response arrives late (some servers pipeline EHLO response after MAIL FROM)
    auto r = run_test({
        "220 mx ESMTP",
        "250-SIZE 10000000\n250 SMTPUTF8",   // EHLO
        "250-STARTTLS\n250-SIZE\n250 SMTPUTF8", // late EHLO (realign)
        "250 Ok",                            // actual MAIL FROM
        "250 Ok", "354 Start", "250 OK"
    });
    assert(r.success);
    std::cout << "  [PASS] mail_from_realign" << std::endl;
}

} // anonymous namespace

int main() {
    std::cout << "Outbound SMTP FSM Test Suite\n==========================\n";
    // 所有测试通过构造函数自动注册和运行
    std::cout << "\nAll tests passed.\n";
    return 0;
}
