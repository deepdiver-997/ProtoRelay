// safe_parse 回归测试（common/safe_parse.h）
//
// 剧情：safe_stoull 曾在 imap_types.hpp / pop3_types.hpp 各放一份副本，SMTP 则裸用
// std::stoi 解析 DB 的 status 列。DB 列 NULL 时 get_value 给 ""，stoi 抛 invalid_argument：
//   - 在 worker 线程（boost::asio::thread_pool 不接异常）→ std::terminate 整进程；
//   - 在 io 线程（RCPT handler 里解析 trusted_relay_networks 的 CIDR 前缀）→ run() 被
//     catch 后线程退出不重进，该 io_context 上全部连接静默僵死。
// 2026-10-17 收口到 safe_parse.h。本测试固定两个函数的口径：
//   safe_stoull —— 宽松，失败归 0（0 本来就是非法值的场景）
//   parse_uint  —— 严格，失败可辨识（CIDR 前缀：把坏值当 /0 = 信任全网 = 开放中继）
#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>

#include "mail_system/back/common/safe_parse.h"

using namespace mail_system;

static int g_pass = 0, g_fail = 0;

static void check(const char* what, bool cond) {
    if (cond) { g_pass++; return; }
    g_fail++;
    std::printf("  FAIL %s\n", what);
}

int main() {
    // ---- safe_stoull：失败归 0，绝不抛 ----
    check("stoull empty -> 0",        safe_stoull("") == 0);
    check("stoull NULL text -> 0",    safe_stoull("NULL") == 0);
    check("stoull alpha -> 0",        safe_stoull("abc") == 0);
    check("stoull 1 -> 1",            safe_stoull("1") == 1);
    check("stoull 42 -> 42",          safe_stoull("42") == 42);
    check("stoull 0 -> 0",            safe_stoull("0") == 0);
    check("stoull overflow -> 0",     safe_stoull("99999999999999999999999") == 0);
    // 既有宽松语义（std::stoull 行为）：前导空白跳过、数字前缀截取。
    // 这是 IMAP 序列号 / POP3 参数解析一直依赖的口径，固定下来防止"顺手改严"。
    check("stoull leading space ok",  safe_stoull(" 7") == 7);
    check("stoull digit prefix",      safe_stoull("12abc") == 12);

    // ---- parse_uint：整串数字才成功，0 与失败可区分 ----
    uint64_t v = 0xdead;
    check("uint empty -> false",      !parse_uint("", v));
    check("uint 24 -> true",          parse_uint("24", v) && v == 24);
    check("uint 0 -> true,0",         parse_uint("0", v) && v == 0);      // 关键：0 是合法值
    check("uint 32 -> true",          parse_uint("32", v) && v == 32);
    check("uint alpha -> false",      !parse_uint("x", v));
    check("uint trailing space",      !parse_uint("24 ", v));
    check("uint leading space",       !parse_uint(" 24", v));
    check("uint plus sign",           !parse_uint("+24", v));
    check("uint minus sign",          !parse_uint("-1", v));
    check("uint digit prefix",        !parse_uint("12abc", v));
    check("uint overflow",            !parse_uint("99999999999999999999999", v));
    check("uint max u64",             parse_uint("18446744073709551615", v) && v == UINT64_MAX);

    // ---- CIDR 场景复刻：坏前缀绝不能被当成 /0 ----
    {
        const char* bad[] = {"", "x", "99x", " 8", "8 "};
        for (const char* s : bad) {
            uint64_t p = 0;
            check((std::string("cidr bad prefix rejected: [") + s + "]").c_str(),
                  !parse_uint(s, p));
        }
    }

    std::printf("safe_parse_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
