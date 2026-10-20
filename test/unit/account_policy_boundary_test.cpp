// AccountPolicy 本域发件人边界纯函数单测（2026-10-20）
// 守什么：SMTP 入站"本域发件人边界防伪造"三条规则的判定核心——
//   规则 1 信封发件人本域判定（envelope_sender_is_local）
//   规则 2 From 头域名本域判定（from_header_is_local / from_header_domain）
//   规则 3 认证提交 From 头与账号精确匹配（from_header_matches_account）
// 这些函数是 DATA_END/MAIL FROM 拒绝的唯一依据，解析错一个尖括号/大小写
// 就要么放过高仿伪造、要么误杀合法提交，故在纯函数层穷举边界。
// FSM 级场景回归见 smtps_fsm_test.cpp（local_sender_* / from_header_* 用例）。
#undef NDEBUG
#include <cassert>  // 在 #undef NDEBUG 后重新包含，确保 assert 生效
#include "mail_system/back/common/account_policy.h"

#include <iostream>
#include <string>

using mail_system::AccountPolicy;

static void test_envelope_sender_is_local() {
    std::cout << "\n=== envelope_sender_is_local（规则 1：MAIL FROM 信封） ===" << std::endl;

    // 本域命中
    assert(AccountPolicy::envelope_sender_is_local("user@scut.email", "scut.email"));
    // 大小写不敏感（发件人侧与配置侧两个方向）
    assert(AccountPolicy::envelope_sender_is_local("User@SCUT.Email", "scut.email"));
    assert(AccountPolicy::envelope_sender_is_local("user@scut.email", "SCUT.EMAIL"));
    // 非本域
    assert(!AccountPolicy::envelope_sender_is_local("attacker@gmail.com", "scut.email"));
    // 相似域不是本域（整段比较，非后缀匹配）
    assert(!AccountPolicy::envelope_sender_is_local("user@fakescut.email", "scut.email"));
    assert(!AccountPolicy::envelope_sender_is_local("user@scut.email.evil.com", "scut.email"));
    // 空信封 <>（退信路径）恒为 false —— FSM 传入尖括号内内容，空串即退信
    assert(!AccountPolicy::envelope_sender_is_local("", "scut.email"));
    // 无 @ 的垃圾输入不判本域
    assert(!AccountPolicy::envelope_sender_is_local("postmaster", "scut.email"));
    // 防御性：带尖括号/空白也能解析（FSM 正常传尖括号内内容，此处兜底）
    assert(AccountPolicy::envelope_sender_is_local("<user@scut.email>", "scut.email"));
    assert(AccountPolicy::envelope_sender_is_local(" user@scut.email ", "scut.email"));
    std::cout << "  [PASS] envelope_sender_is_local" << std::endl;
}

static void test_from_header_domain() {
    std::cout << "\n=== from_header_domain / from_header_address（From 头值解析） ===" << std::endl;

    // 纯 addr-spec
    assert(AccountPolicy::from_header_domain("user@scut.email") == "scut.email");
    assert(AccountPolicy::from_header_address("user@scut.email") == "user@scut.email");
    // 带尖括号
    assert(AccountPolicy::from_header_domain("<user@scut.email>") == "scut.email");
    assert(AccountPolicy::from_header_address("<user@scut.email>") == "user@scut.email");
    // 带显示名（DMARC 对齐与边界判定最常见的形态）
    assert(AccountPolicy::from_header_domain("Zhang San <user@scut.email>") == "scut.email");
    assert(AccountPolicy::from_header_address("Zhang San <user@scut.email>") == "user@scut.email");
    // 域名统一小写
    assert(AccountPolicy::from_header_domain("User@SCUT.Email") == "scut.email");
    // addr-spec 保持原样（大小写由比较函数归一）
    assert(AccountPolicy::from_header_address("User@SCUT.Email") == "User@SCUT.Email");
    // 只有单边尖括号 → 按原值兜底解析（与 InboundVerifier 既有行为一致）
    assert(AccountPolicy::from_header_domain("<user@scut.email") == "scut.email");
    // 无法解析 → 空域名
    assert(AccountPolicy::from_header_domain("plain-name").empty());
    assert(AccountPolicy::from_header_domain("").empty());
    assert(AccountPolicy::from_header_domain("<>").empty());
    assert(AccountPolicy::from_header_domain("user@").empty());
    std::cout << "  [PASS] from_header_domain / from_header_address" << std::endl;
}

static void test_from_header_is_local() {
    std::cout << "\n=== from_header_is_local（规则 2：From 头域名本域判定） ===" << std::endl;

    assert(AccountPolicy::from_header_is_local("user@scut.email", "scut.email"));
    assert(AccountPolicy::from_header_is_local("Zhang San <User@Scut.Email>", "scut.email"));
    // 配置域大小写归一
    assert(AccountPolicy::from_header_is_local("user@scut.email", "SCUT.EMAIL"));
    // 非本域 / 相似域
    assert(!AccountPolicy::from_header_is_local("a@gmail.com", "scut.email"));
    assert(!AccountPolicy::from_header_is_local("a@fakescut.email", "scut.email"));
    // From 缺失 / 无法解析 → false（不因本规则拒收）
    assert(!AccountPolicy::from_header_is_local("", "scut.email"));
    assert(!AccountPolicy::from_header_is_local("no-address-here", "scut.email"));
    std::cout << "  [PASS] from_header_is_local" << std::endl;
}

static void test_from_header_matches_account() {
    std::cout << "\n=== from_header_matches_account（规则 3：认证提交 From 归属） ===" << std::endl;

    // 精确相等（大小写不敏感、容忍显示名/尖括号）
    assert(AccountPolicy::from_header_matches_account("user0@scut.email", "user0@scut.email"));
    assert(AccountPolicy::from_header_matches_account("User0@SCUT.Email", "user0@scut.email"));
    assert(AccountPolicy::from_header_matches_account("Zhang San <user0@scut.email>", "user0@scut.email"));
    assert(AccountPolicy::from_header_matches_account("<user0@scut.email>", "USER0@SCUT.EMAIL"));
    // 不同账号 → 不匹配
    assert(!AccountPolicy::from_header_matches_account("other@scut.email", "user0@scut.email"));
    // 本域 local part 不同（冒充同域同事）
    assert(!AccountPolicy::from_header_matches_account("Zhang San <user1@scut.email>", "user0@scut.email"));
    // 任一侧无法解析 → false（调用方据此跳过而非拒绝）
    assert(!AccountPolicy::from_header_matches_account("", "user0@scut.email"));
    assert(!AccountPolicy::from_header_matches_account("unsigned", "user0@scut.email"));
    // 账号名为空（不应出现，防御）→ false
    assert(!AccountPolicy::from_header_matches_account("user0@scut.email", ""));
    std::cout << "  [PASS] from_header_matches_account" << std::endl;
}

static void test_account_in_alias_targets() {
    std::cout << "\n=== account_in_alias_targets（规则 3 别名分支集合判定） ===" << std::endl;

    assert(AccountPolicy::account_in_alias_targets({"user1@scut.email", "user0@scut.email"},
                                                   "user0@scut.email"));
    // 大小写 / 空白归一（DB 目标与登录名书写不一致时仍命中）
    assert(AccountPolicy::account_in_alias_targets({"User0@Scut.Email"}, "user0@scut.email"));
    assert(AccountPolicy::account_in_alias_targets({" user0@scut.email "}, "user0@scut.email"));
    // 未命中
    assert(!AccountPolicy::account_in_alias_targets({"user1@scut.email", "user2@scut.email"},
                                                    "user0@scut.email"));
    // 空集合（非别名 / DB 不可用 / 查询失败）→ false = fail-closed 不匹配
    assert(!AccountPolicy::account_in_alias_targets({}, "user0@scut.email"));
    // 登录名为空（防御）→ false
    assert(!AccountPolicy::account_in_alias_targets({"user0@scut.email"}, ""));
    std::cout << "  [PASS] account_in_alias_targets" << std::endl;
}

int main() {
    std::cout << "AccountPolicy Local-Sender Boundary Test Suite\n"
              << "===============================================" << std::endl;

    test_envelope_sender_is_local();
    test_from_header_domain();
    test_from_header_is_local();
    test_from_header_matches_account();
    test_account_in_alias_targets();

    std::cout << "\nAll tests passed.\n" << std::endl;
    return 0;
}
