#include "mail_system/back/mailServer/fsm/imaps/imap_utils.h"
#include "mail_system/back/mailServer/fsm/imaps/imap_types.hpp"   // safe_stoull
#include "mail_system/back/common/mime_parser.h"   // MimePart / parse_mime_tree
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <set>
#include <sstream>

namespace mail_system {
namespace imap_utils {

namespace {
// modified Base64 编码表（用 ',' 代替 '/', 不加 '=' 填充）
const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+,";

// modified Base64 解码表（用 ',' 代替 '/'）
const int kBase64Decode[128] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,63,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
};
} // namespace

// RFC 3501 date-time: DD-Mon-YYYY HH:MM:SS +ZZZZ
std::string imap_timestamp(time_t t) {
    struct tm result;
    memset(&result, 0, sizeof(result));
#ifdef _WIN32
    gmtime_s(&result, &t);
#else
    gmtime_r(&t, &result);
#endif
    static const char* months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    char buf[64];
    snprintf(buf, sizeof(buf), "%02d-%s-%04d %02d:%02d:%02d +0000",
             result.tm_mday, months[result.tm_mon],
             result.tm_year + 1900,
             result.tm_hour, result.tm_min, result.tm_sec);
    return std::string(buf);
}

// RFC 3501 quoted-string
// 注意：IMAP-UTF-7 编码的名称（以 & 开头）也需要加引号，
// 否则 VMime 等客户端会把 &...- 中的 '-' 当作名称的一部分而非编码结束符
std::string quote_string(const std::string& s) {
    // Always quote if: empty, contains special chars, or is IMAP-UTF-7 encoded
    // IMAP-UTF-7 编码的名称（以 & 开头）需要加引号，
    // 纯 ASCII atom 不加引号
    bool needs_quote = s.empty() ||
                       s[0] == '&' ||
                       s.find_first_of("\"\\") != std::string::npos ||
                       s.find(' ') != std::string::npos;
    if (!needs_quote) {
        return s;
    }
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

// UTF-8 → modified UTF-7（用于 LIST/LSUB 响应）
std::string encode_mailbox_name(const std::string& name) {
    std::string upper = name;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
    if (upper == "INBOX") return "INBOX";

    std::string result;
    std::vector<uint16_t> buf;

    auto flush = [&]() {
        if (buf.empty()) return;
        std::vector<uint8_t> be;
        for (uint16_t u : buf) {
            be.push_back((u >> 8) & 0xFF);
            be.push_back(u & 0xFF);
        }
        result += '&';
        for (size_t i = 0; i < be.size(); i += 3) {
            uint8_t b[3] = {0}; int n = 0;
            for (int j = 0; j < 3 && i + j < be.size(); ++j, ++n) b[j] = be[i + j];
            uint32_t t = (b[0] << 16) | (b[1] << 8) | b[2];
            result += kBase64[(t >> 18) & 0x3F];
            result += kBase64[(t >> 12) & 0x3F];
            if (n >= 2) result += kBase64[(t >> 6) & 0x3F];
            if (n >= 3) result += kBase64[t & 0x3F];
        }
        result += '-';
        buf.clear();
    };

    size_t i = 0;
    while (i < name.size()) {
        unsigned char c = static_cast<unsigned char>(name[i]);
        if (c < 0x80) {
            flush();
            result += (c == '&') ? "&-" : std::string(1, static_cast<char>(c));
            i++;
        } else {
            uint32_t cp = 0; size_t extra = 0;
            if (c >= 0xF0)      { cp = c & 0x07; extra = 3; }
            else if (c >= 0xE0) { cp = c & 0x0F; extra = 2; }
            else                { cp = c & 0x1F; extra = 1; }
            for (size_t j = 1; j <= extra; ++j)
                cp = (cp << 6) | (static_cast<unsigned char>(name[i + j]) & 0x3F);
            i += extra + 1;
            if (cp > 0xFFFF) {
                cp -= 0x10000;
                buf.push_back(0xD800 | ((cp >> 10) & 0x3FF));
                buf.push_back(0xDC00 | (cp & 0x3FF));
            } else {
                buf.push_back(static_cast<uint16_t>(cp));
            }
        }
    }
    flush();
    return result;
}

// modified UTF-7 → UTF-8（手动实现，不依赖 iconv）
// 处理 &base64- 序列以及 &- 转义
std::string decode_mailbox_name(const std::string& imap7) {
    std::string result;

    size_t i = 0;
    while (i < imap7.size()) {
        char c = imap7[i];

        if (c == '&') {
            if (i + 1 < imap7.size() && imap7[i + 1] == '-') {
                // &- → 字面 '&'
                result += '&';
                i += 2;
            } else {
                // &...- → modified Base64 片段
                size_t end = imap7.find('-', i + 1);
                if (end == std::string::npos) {
                    result += c;
                    i++;
                    continue;
                }
                std::string b64 = imap7.substr(i + 1, end - i - 1);
                i = end + 1;

                // 解码 modified Base64 → 字节
                std::vector<uint8_t> bytes;
                uint32_t acc = 0;
                int bits = 0;
                for (char bc : b64) {
                    if (bc < 0 || bc >= 128) continue;
                    int val = kBase64Decode[static_cast<int>(bc)];
                    if (val < 0) continue;
                    acc = (acc << 6) | static_cast<uint32_t>(val);
                    bits += 6;
                    if (bits >= 8) {
                        bits -= 8;
                        bytes.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
                    }
                }

                // UTF-16BE → UTF-8
                for (size_t j = 0; j + 1 < bytes.size(); j += 2) {
                    uint16_t unit = (static_cast<uint16_t>(bytes[j]) << 8)
                                   | bytes[j + 1];

                    if (unit >= 0xD800 && unit <= 0xDBFF && j + 3 < bytes.size()) {
                        // 高 surrogate + 低 surrogate
                        uint16_t low = (static_cast<uint16_t>(bytes[j + 2]) << 8)
                                      | bytes[j + 3];
                        uint32_t cp = 0x10000
                            + ((unit - 0xD800) << 10)
                            + (low - 0xDC00);
                        result += static_cast<char>(0xF0 | ((cp >> 18) & 0x07));
                        result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                        result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        result += static_cast<char>(0x80 | (cp & 0x3F));
                        j += 2; // 跳过低 surrogate
                    } else if (unit >= 0xD800 && unit <= 0xDFFF) {
                        continue; // 孤立的 surrogate，跳过
                    } else if (unit < 0x80) {
                        result += static_cast<char>(unit);
                    } else if (unit < 0x800) {
                        result += static_cast<char>(0xC0 | ((unit >> 6) & 0x1F));
                        result += static_cast<char>(0x80 | (unit & 0x3F));
                    } else {
                        result += static_cast<char>(0xE0 | ((unit >> 12) & 0x0F));
                        result += static_cast<char>(0x80 | ((unit >> 6) & 0x3F));
                        result += static_cast<char>(0x80 | (unit & 0x3F));
                    }
                }
            }
        } else {
            result += c;
            i++;
        }
    }
    return result;
}

// IMAP-UTF-7 解码（与 decode_mailbox_name 原为重复实现，语义一致，委托过去）
std::string decode_imap_utf7(const std::string& imap7) {
    return decode_mailbox_name(imap7);
}

// 构建 flags 字符串
std::string build_flags_string(int status, bool starred, bool deleted, bool important) {
    std::string flags;
    // \Seen: status==0 (已读)
    if (status == 0) {
        flags += "\\Seen ";
    }
    if (starred) {
        flags += "\\Flagged ";
    }
    if (deleted) {
        flags += "\\Deleted ";
    }
    if (important) {
        flags += "\\Important ";
    }
    // 去除尾部空格
    if (!flags.empty() && flags.back() == ' ') {
        flags.pop_back();
    }
    return flags;
}

// 构建 ENVELOPE 响应
std::string build_envelope_string(
    const std::string& date_str,
    const std::string& subject,
    const std::string& from,
    const std::string& sender,
    const std::string& reply_to,
    const std::string& to,
    const std::string& cc,
    const std::string& bcc,
    const std::string& in_reply_to,
    const std::string& message_id)
{
    // ENVELOPE( date, subject, from, sender, reply-to, to, cc, bcc, in-reply-to, message-id )
    // 每个地址列表是: ((name NIL addr host) ...)
    auto addr_to_list = [](const std::string& addr) -> std::string {
        if (addr.empty()) return "NIL";
        // Parse "user@domain" or "Name <user@domain>"
        std::string name = "NIL";
        std::string user;
        std::string domain;
        std::string input = addr;

        // Try to extract name part
        size_t angle_start = input.find('<');
        size_t angle_end = input.find('>');
        std::string addr_spec;
        if (angle_start != std::string::npos && angle_end != std::string::npos) {
            std::string before_angle = input.substr(0, angle_start);
            // trim
            before_angle.erase(0, before_angle.find_first_not_of(" \t\""));
            before_angle.erase(before_angle.find_last_not_of(" \t\"") + 1);
            if (!before_angle.empty()) {
                name = "\"" + before_angle + "\"";
            }
            addr_spec = input.substr(angle_start + 1, angle_end - angle_start - 1);
        } else {
            addr_spec = input;
        }

        size_t at_pos = addr_spec.find('@');
        if (at_pos != std::string::npos) {
            user = addr_spec.substr(0, at_pos);
            domain = addr_spec.substr(at_pos + 1);
        } else {
            user = addr_spec;
            domain = "";
        }

        if (user.empty()) user = "NIL";
        if (domain.empty()) domain = "NIL";

        // ENVELOPE address fields MUST be quoted strings per RFC 3501 nstring
        auto force_quote = [](const std::string& s) -> std::string {
            return "\"" + s + "\"";
        };
        return "((" + name + " NIL " + force_quote(user) + " " + force_quote(domain) + "))";
    };

    std::string out = "(";
    out += (date_str.empty() ? "NIL" : "\"" + date_str + "\"") + " ";
    out += (subject.empty() ? "NIL" : "\"" + subject + "\"") + " ";
    out += addr_to_list(from) + " ";
    out += addr_to_list(sender.empty() ? from : sender) + " ";
    out += (reply_to.empty() ? addr_to_list(from) : addr_to_list(reply_to)) + " ";
    out += addr_to_list(to) + " ";
    out += (cc.empty() ? "NIL" : addr_to_list(cc)) + " ";
    out += (bcc.empty() ? "NIL" : addr_to_list(bcc)) + " ";
    out += (in_reply_to.empty() ? "NIL" : "\"" + in_reply_to + "\"") + " ";
    out += (message_id.empty() ? "NIL" : "\"" + message_id + "\"");
    out += ")";
    return out;
}

// 构建 BODY[] 响应
std::string build_fetch_body_response(const std::string& body_content, size_t octets) {
    (void)octets;
    if (body_content.empty()) {
        return "\"\"";
    }
    // Use literal: {size}\r\n<content>
    std::string out = "{" + std::to_string(body_content.size()) + "}\r\n";
    out += body_content;
    return out;
}

// BODYSTRUCTURE 构建（RFC 3501 §7.4.2）
std::string build_bodystructure(const std::string& raw) {
    // 旧邮件回退路径（无预解析 MIME 树时使用）：用 parse_mime_tree 解析，
    // 而非硬编码 text/plain（否则 HTML 邮件会被按纯文本渲染）。
    MimePart root;
    parse_mime_tree(raw, root);
    return build_bodystructure_tree(root, raw);
}

// 取 part 自己的 MIME 头块（含于 [offset, offset+length) 区间，不含空行分隔符）。
// 兼容裸 LF 邮件（\n\n 分隔）。
std::string mime_part_header_block(const std::string& raw, const MimePart& part) {
    size_t start = part.offset;
    size_t end = part.offset + part.length;
    if (start >= raw.size()) return {};
    if (end > raw.size()) end = raw.size();
    if (end <= start) return {};

    size_t sep = raw.find("\r\n\r\n", start);
    if (sep == std::string::npos || sep + 4 > end) {
        sep = raw.find("\n\n", start);
        if (sep == std::string::npos || sep + 2 > end) return {};
        return raw.substr(start, sep + 1 - start);   // 保留最后一个头行自身的 \n
    }
    return raw.substr(start, sep + 2 - start);       // 保留最后一个头行自身的 \r\n
}

// 头块按字段名过滤（大小写不敏感，支持 folded 续行随主行去留）。
std::string filter_header_block(const std::string& headers,
                                const std::string& fields_csv, bool exclude) {
    std::set<std::string> wanted;
    {
        std::istringstream fs(fields_csv);
        std::string f;
        while (std::getline(fs, f, ',')) {
            f = mime_params_detail::trim(f);
            if (f.empty()) continue;
            f = mime_params_detail::lower(f);
            // 兼容 "Subject From"（空格分隔）与 "Subject, From"（逗号分隔）两种写法
            std::istringstream ws(f);
            std::string w;
            while (ws >> w) wanted.insert(mime_params_detail::lower(w));
        }
    }
    std::string filtered;
    size_t pos = 0;
    bool keep_current = false;
    std::string current_line;
    while (pos < headers.size()) {
        size_t eol = headers.find('\n', pos);
        if (eol == std::string::npos) eol = headers.size();
        std::string line = headers.substr(pos, eol - pos);
        pos = eol == headers.size() ? headers.size() : eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        const bool folded = !line.empty() && (line.front() == ' ' || line.front() == '\t');
        if (!folded) {
            if (keep_current && !current_line.empty()) {
                filtered += current_line + "\r\n";
            }
            current_line = line;
            if (line.empty()) { keep_current = false; continue; }   // 头块结束（空行）
            size_t colon = line.find(':');
            if (colon == std::string::npos) { keep_current = exclude; continue; }
            const bool hit = wanted.count(mime_params_detail::lower(line.substr(0, colon))) > 0;
            keep_current = exclude ? !hit : hit;
        } else if (keep_current) {
            current_line += "\r\n" + line;   // 续行保持原样（含前导空白）
        }
    }
    if (keep_current && !current_line.empty()) filtered += current_line + "\r\n";
    // 去掉末尾最后一个 CRLF（与既有 BODY[HEADER.FIELDS] 行为一致）
    if (!filtered.empty() && filtered.size() >= 2)
        filtered.resize(filtered.size() - 2);
    return filtered;
}

namespace {
// 内嵌消息的 ENVELOPE：从其头块抽 RFC 3501 §7.4.2 的十个字段。
std::string build_envelope_from_headers(const std::string& header_block) {
    auto hv = [&](const char* name) {
        return mime_header_value(header_block, name);
    };
    return build_envelope_string(
        hv("date"), hv("subject"), hv("from"), hv("sender"), hv("reply-to"),
        hv("to"), hv("cc"), hv("bcc"), hv("in-reply-to"), hv("message-id"));
}
} // namespace

bool mime_tree_has_rfc822(const MimePart& mp) {
    if (mp.type == "message" && mp.subtype == "rfc822") return true;
    for (const auto& sub : mp.subs)
        if (mime_tree_has_rfc822(sub)) return true;
    return false;
}

const MimePart* resolve_mime_part(const MimePart& root,
                                  const std::vector<uint64_t>& path) {
    const MimePart* cur = &root;
    for (size_t depth = 0; depth < path.size(); ++depth) {
        const uint64_t idx = path[depth];
        if (cur->is_multipart()) {
            if (idx == 0 || idx > cur->subs.size()) return nullptr;
            cur = &cur->subs[static_cast<size_t>(idx - 1)];
            continue;
        }
        if (cur->type == "message" && cur->subtype == "rfc822" && !cur->subs.empty()) {
            // 下一级编号落在内嵌消息上：内嵌为 multipart 时不占编号，
            // 直接落其子部件；内嵌为叶子时 1 即该叶子。
            const MimePart& embedded = cur->subs[0];
            if (embedded.is_multipart()) {
                if (idx == 0 || idx > embedded.subs.size()) return nullptr;
                cur = &embedded.subs[static_cast<size_t>(idx - 1)];
            } else {
                if (idx != 1) return nullptr;
                cur = &embedded;
            }
            continue;
        }
        // 非 multipart 根（单 part 邮件）：仅顶层第一级编号 1 指向整封（既有行为）；
        // 更深层路径（如 1.1）无处可去 → nullptr
        if (depth == 0 && cur == &root && idx == 1) continue;
        return nullptr;
    }
    return cur;
}

bool parse_body_section(const std::string& inside, ImapBodySection& out) {
    out = ImapBodySection{};
    out.inside = inside;
    if (inside.empty()) return true;   // BODY[]：整封

    // 路径：数字 + '.' 分隔（≥1）
    size_t i = 0;
    while (i < inside.size()) {
        size_t digits = i;
        while (digits < inside.size() &&
               std::isdigit(static_cast<unsigned char>(inside[digits]))) ++digits;
        if (digits == i) return false;   // 不是数字开头（如 HEADER —— 顶层头走旧路径）
        uint64_t n = 0;
        for (size_t k = i; k < digits; ++k) {
            n = n * 10 + static_cast<uint64_t>(inside[k] - '0');
            if (n > 1000000000ull) return false;   // 防御荒谬编号
        }
        if (n == 0) return false;   // section 编号从 1 起
        out.path.push_back(n);
        i = digits;
        if (i < inside.size() && inside[i] == '.') {
            size_t digits2 = i + 1;
            if (digits2 >= inside.size() ||
                !std::isdigit(static_cast<unsigned char>(inside[digits2]))) break;
            ++i;   // 消费 '.'，继续路径
        } else {
            break;
        }
    }
    if (i >= inside.size()) return true;   // 纯路径，kind=full

    // 后缀：.MIME / .HEADER / .TEXT / .HEADER.FIELDS (.NOT) (…)
    std::string rest = mime_params_detail::trim(inside.substr(i));
    std::transform(rest.begin(), rest.end(), rest.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (rest == ".MIME") { out.kind = ImapBodySection::Kind::mime; return true; }
    if (rest == ".HEADER") { out.kind = ImapBodySection::Kind::header; return true; }
    if (rest == ".TEXT") { out.kind = ImapBodySection::Kind::text; return true; }
    constexpr const char* kHF = ".HEADER.FIELDS";
    constexpr const char* kHFN = ".HEADER.FIELDS.NOT";
    std::string fields;
    if (rest.rfind(kHFN, 0) == 0) {
        out.kind = ImapBodySection::Kind::header_fields_not;
        fields = inside.substr(i + std::char_traits<char>::length(kHFN));
    } else if (rest.rfind(kHF, 0) == 0) {
        out.kind = ImapBodySection::Kind::header_fields;
        fields = inside.substr(i + std::char_traits<char>::length(kHF));
    } else {
        return false;
    }
    // 字段列表：(…) 括号内原文；容忍括号前空白
    fields = mime_params_detail::trim(fields);
    if (fields.size() < 2 || fields.front() != '(' || fields.back() != ')') return false;
    out.fields = mime_params_detail::trim(fields.substr(1, fields.size() - 2));
    return true;
}

std::string extract_body_section(const std::string& raw, const MimePart& root,
                                 const ImapBodySection& section) {
    if (section.path.empty()) {
        // 整封 / 顶层 header / text（BODY[]、BODY[HEADER]、BODY[TEXT]）
        switch (section.kind) {
            case ImapBodySection::Kind::full:
                return raw;
            case ImapBodySection::Kind::header:
            case ImapBodySection::Kind::header_fields:
            case ImapBodySection::Kind::header_fields_not: {
                MimePart whole;
                whole.offset = 0;
                whole.length = static_cast<std::uint32_t>(raw.size());
                std::string block = mime_part_header_block(raw, whole);
                if (section.kind != ImapBodySection::Kind::header)
                    block = filter_header_block(block, section.fields,
                                                section.kind == ImapBodySection::Kind::header_fields_not);
                return block;
            }
            case ImapBodySection::Kind::text: {
                MimePart whole;
                whole.offset = 0;
                whole.length = static_cast<std::uint32_t>(raw.size());
                return extract_part_content(raw, whole);
            }
            case ImapBodySection::Kind::mime:
                return {};
        }
        return {};
    }

    const MimePart* part = resolve_mime_part(root, section.path);
    if (!part) return {};

    switch (section.kind) {
        case ImapBodySection::Kind::full:
            return extract_part_content(raw, *part);
        case ImapBodySection::Kind::mime:
            return mime_part_header_block(raw, *part);
        case ImapBodySection::Kind::header:
        case ImapBodySection::Kind::header_fields:
        case ImapBodySection::Kind::header_fields_not:
        case ImapBodySection::Kind::text: {
            // HEADER/TEXT 指内嵌消息的头/正文（RFC 3501 §6.4.5：part n 为
            // message/rfc822 时，HEADER/HEADER.FIELDS/TEXT 作用于内嵌消息）
            if (part->type != "message" || part->subtype != "rfc822" ||
                part->subs.empty()) {
                return {};
            }
            const MimePart& embedded = part->subs[0];
            if (section.kind == ImapBodySection::Kind::text)
                return extract_part_content(raw, embedded);
            std::string block = mime_part_header_block(raw, embedded);
            if (section.kind != ImapBodySection::Kind::header)
                block = filter_header_block(block, section.fields,
                                            section.kind == ImapBodySection::Kind::header_fields_not);
            return block;
        }
    }
    return {};
}

std::string build_bodystructure_tree(const MimePart& mp) {
    return build_bodystructure_tree(mp, std::string());
}

std::string build_bodystructure_tree(const MimePart& mp, const std::string& raw) {
    // RFC 3501 string：非 ASCII/换行使用 literal，引用串转义引号与反斜杠。
    const auto string = [](const std::string& value) {
        if (std::any_of(value.begin(), value.end(), [](unsigned char c) {
                return c >= 128 || c == '\r' || c == '\n';
            })) return "{" + std::to_string(value.size()) + "}\r\n" + value;
        std::string out = "\"";
        for (char c : value) { if (c == '\\' || c == '"') out += '\\';out += c; }
        return out + "\"";
    };
    const auto disposition = [&]() {
        if (mp.disposition.empty()) return std::string("NIL");
        return "(" + string(mp.disposition) + " " +
            (mp.name.empty() ? "NIL" : "(\"FILENAME\" " + string(mp.name) + ")") + ")";
    };
    const auto params_of = [&]() {
        std::string params;
        if (!mp.charset.empty()) params += "\"CHARSET\" " + string(mp.charset);
        if (!mp.name.empty()) { if (!params.empty()) params += " ";params += "\"NAME\" " + string(mp.name); }
        return params.empty() ? std::string(" NIL") : " (" + params + ")";
    };
    if (mp.is_multipart()) {
        // RFC 3501 §7.4.2: multipart 子 part 在最前，subtype 在最后
        //   ((sub1)(sub2) "alternative" ("BOUNDARY" "...") NIL NIL NIL)
        std::string result = "(";
        for (const auto& sub : mp.subs)
            result += build_bodystructure_tree(sub, raw);
        result += " " + string(mp.subtype);
        if (!mp.boundary.empty())
            result += " (\"BOUNDARY\" " + string(mp.boundary) + ")";
        else
            result += " NIL";
        result += " " + disposition() + " NIL NIL)";
        return result;
    }
    // message/rfc822：基础字段之后必须附加 内嵌消息 ENVELOPE + 内嵌 BODYSTRUCTURE
    // + 内嵌正文行数，然后才是扩展字段（md5/disposition/language/location）。
    // 之前被当普通叶子输出，客户端按位置解析字段错位、rfc822 附件渲染失败。
    if (mp.type == "message" && mp.subtype == "rfc822") {
        std::string result = "(" + string(mp.type) + " " + string(mp.subtype);
        result += params_of();
        result += mp.content_id.empty() ? " NIL" : " " + string(mp.content_id);
        result += " NIL " + string(mp.encoding) + " " + std::to_string(mp.body_size);
        if (!mp.subs.empty()) {
            const MimePart& embedded = mp.subs[0];
            result += " " + build_envelope_from_headers(mime_part_header_block(raw, embedded));
            result += " " + build_bodystructure_tree(embedded, raw);
            result += " " + std::to_string(mp.lines);
        } else {
            // 截断/无正文：输出空 text/plain 占位，字段数保持 RFC 形状
            result += " NIL (\"text\" \"plain\" NIL NIL NIL \"7bit\" 0 0) 0";
        }
        result += " NIL " + disposition() + " NIL NIL)";
        return result;
    }
    std::string result = "(" + string(mp.type) + " " + string(mp.subtype);
    result += params_of();
    result += mp.content_id.empty() ? " NIL" : " " + string(mp.content_id);
    result += " NIL " + string(mp.encoding) + " " + std::to_string(mp.body_size);
    if (mp.type == "text") result += " " + std::to_string(mp.lines);
    result += " NIL " + disposition() + " NIL NIL)";
    return result;
}

// 展开逗号分隔的序列号集（支持 "1" / "1:*" / "1,3,5" / "1:3,5" / "*"）
// 输出到 ranges（(start,end) 闭区间列表，已 clamp 到 [1,total]）
void expand_seq_set(const std::string& seq_set, size_t total,
                    std::vector<std::pair<uint64_t, uint64_t>>& ranges) {
    ranges.clear();
    std::istringstream ss(seq_set);
    std::string token;
    while (std::getline(ss, token, ',')) {
        if (token.empty()) continue;
        uint64_t s, e;
        if (token.find(':') != std::string::npos) {
            size_t c = token.find(':');
            std::string a = token.substr(0, c), b = token.substr(c + 1);
            s = (a == "*") ? 1 : safe_stoull(a);
            e = (b == "*") ? total : std::min((uint64_t)safe_stoull(b), (uint64_t)total);
        } else if (token == "*") {
            s = 1;
            e = total;
        } else {
            s = safe_stoull(token);
            e = s;
        }
        if (s < 1) s = 1;
        if (e > total) e = total;
        if (s <= e) ranges.emplace_back(s, e);
    }
}

// 数值 ID 列表 → "a,b,c"（批量 IN 语句用；全数值，无注入面）
std::string join_mail_ids(const std::vector<uint64_t>& ids) {
    std::string out;
    bool first = true;
    for (uint64_t id : ids) {
        if (!first) out += ",";
        out += std::to_string(id);
        first = false;
    }
    return out;
}

} // namespace imap_utils
} // namespace mail_system
