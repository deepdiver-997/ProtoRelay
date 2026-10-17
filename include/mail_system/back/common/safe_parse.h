#ifndef MAIL_SYSTEM_SAFE_PARSE_H
#define MAIL_SYSTEM_SAFE_PARSE_H

#include <charconv>
#include <cstdint>
#include <stdexcept>   // std::invalid_argument / out_of_range
// ⚠ 必须显式包含：macOS libc++ 会经 <string> 传递包含 <stdexcept>，Linux libstdc++ 不会，
// 交叉编译时 catch(std::invalid_argument&) 直接报 expected unqualified-id。2026-08-29
#include <string>
#include <string_view>
#include <system_error>

namespace mail_system {

// 宽松数字解析：空串 / 非数字 / 溢出一律返回 0，绝不抛。
// 适用于"0 本来就表示非法/无效"的场景——DB 行的 status/id（0 = 禁用/不存在，fail-closed）、
// IMAP 序列号、POP3 RETR/DELE 参数（RFC 1939 旧客户端常带括号备注，0 = 非法）。
// ⚠ 不适用于"0 是合法值且必须与解析失败区分"的场景（如 CIDR 前缀：/0 = 匹配全网），
//   那种用下面的 parse_uint。
//
// 历史：曾在 imap_types.hpp 与 pop3_types.hpp 各放一份同名 inline（"避免互相 include"），
// SMTP 则裸用 std::stoi —— DB 列为 NULL 时 get_value 给 ""，stoi 抛 invalid_argument，
// 在 worker 线程（boost::asio::thread_pool，不接异常）里无人捕获 → std::terminate 整进程。
// 2026-10-17 三处收口到此。
inline uint64_t safe_stoull(const std::string& s) {
    if (s.empty()) return 0;
    try { return std::stoull(s); }
    catch (const std::invalid_argument&) { return 0; }
    catch (const std::out_of_range&)     { return 0; }
}

// 严格无符号十进制解析：整串必须是数字（不接受前导空白 / 正负号 / 尾随垃圾），
// 成功返回 true 并写 out。用于失败必须可辨识的场景——CIDR 前缀若把 "10.0.0.0/x"
// 宽松解析成 /0，等于信任全网 = 开放中继。
inline bool parse_uint(std::string_view s, uint64_t& out) {
    if (s.empty()) return false;
    const char* first = s.data();
    const char* last  = first + s.size();
    auto [ptr, ec] = std::from_chars(first, last, out, 10);
    return ec == std::errc() && ptr == last;
}

} // namespace mail_system

#endif // MAIL_SYSTEM_SAFE_PARSE_H
