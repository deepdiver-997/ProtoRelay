#ifndef MAIL_SYSTEM_IMAP_UTILS_H
#define MAIL_SYSTEM_IMAP_UTILS_H

// IMAP FSM 的纯工具函数（非模板）——从 traditional_imaps_fsm.tpp 拆出。
// 全部与 ConnectionType 无关：非模板 .cpp 只编译一次（原来 .tpp 每个
// ConnectionType 实例化都编译一遍），可独立单测。
//
// 含：RFC 3501 响应构建（timestamp/quoted-string/ENVELOPE/BODYSTRUCTURE/FLAGS）、
// modified UTF-7 编解码、序列号集/ID 列表助手。

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct MimePart;   // 定义在 common/mime_parser.h（.cpp 里 include）

namespace mail_system {
namespace imap_utils {

// RFC 3501 date-time: DD-Mon-YYYY HH:MM:SS +ZZZZ
std::string imap_timestamp(time_t t);

// RFC 3501 quoted-string（IMAP-UTF-7 编码名（& 开头）也要加引号）
std::string quote_string(const std::string& s);

// modified UTF-7 编解码（RFC 3501 §5.1.3）
std::string encode_mailbox_name(const std::string& name);
std::string decode_mailbox_name(const std::string& imap7);
std::string decode_imap_utf7(const std::string& imap7);

// 响应片段构建
std::string build_flags_string(int status, bool starred, bool deleted, bool important);
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
    const std::string& message_id);
std::string build_fetch_body_response(const std::string& body_content, size_t octets);

// BODYSTRUCTURE 构建（RFC 3501 §7.4.2）
// part 正文提取 extract_part_content 已上移 common/mime_parser.h（IMAP/webmail 共用）
// 单参版本供旧调用方/测试用（raw 为空时 rfc822 内嵌 ENVELOPE 输出全 NIL）。
std::string build_bodystructure(const std::string& raw);
std::string build_bodystructure_tree(const MimePart& mp);
std::string build_bodystructure_tree(const MimePart& mp, const std::string& raw);

// ---------- BODY[...] section 取址（RFC 3501 §6.4.5） ----------

struct ImapBodySection {
    // part 路径，如 {2} / {2,1}；空 = 整封（BODY[]）
    std::vector<uint64_t> path;
    enum class Kind { full, mime, header, header_fields, header_fields_not, text };
    Kind kind = Kind::full;
    std::string fields;   // HEADER.FIELDS(.NOT) 的字段列表原文（括号内）
    std::string inside;   // 方括号内原文（响应标签原样回显）

    bool is_header_kind() const {
        return kind == Kind::header || kind == Kind::header_fields ||
               kind == Kind::header_fields_not;
    }
};

// 解析 BODY[...] 方括号内的 section 文本（如 "2"、"2.1.MIME"、"2.HEADER.FIELDS (Subject)"）。
// 语法非法返回 false（调用方回 BAD）。
bool parse_body_section(const std::string& inside, ImapBodySection& out);

// 按路径语义定位 part：
//   - multipart part 的下一级编号 → 其 subs 成员；
//   - message/rfc822 part 的下一级编号 → 内嵌消息（内嵌为 multipart 时再落到
//     其 subs —— 内嵌 multipart 根不占编号；内嵌为叶子时仅接受 1）；
//   - 非 multipart 根（单 part 邮件）的顶层编号 1 → 根本身（既有行为）。
// 越界/叶子下再深入返回 nullptr。
const MimePart* resolve_mime_part(const MimePart& root,
                                  const std::vector<uint64_t>& path);

// 取 BODY[section] 的数据（原始编码，不做 transfer-decode）。
// header 类 section 指向内嵌消息的头/正文；part 不是 message/rfc822 时为空。
std::string extract_body_section(const std::string& raw, const MimePart& root,
                                 const ImapBodySection& section);

// part 自己的 MIME 头块（BODY[n.MIME]，不含头后的空行分隔符）
std::string mime_part_header_block(const std::string& raw, const MimePart& part);

// 头块按字段名过滤（大小写不敏感；fields_csv 逗号/空白分隔）。
// exclude=false 保留命中字段，true 保留未命中字段（HEADER.FIELDS.NOT）。
std::string filter_header_block(const std::string& headers,
                                const std::string& fields_csv, bool exclude);

// 树中是否含 message/rfc822 part（决定 BODYSTRUCTURE 是否必须读正文取内嵌 ENVELOPE）
bool mime_tree_has_rfc822(const MimePart& mp);

// 展开逗号分隔的序列号集（"1" / "1:*" / "1,3,5" / "1:3,5" / "*"）→ (start,end)
// 闭区间列表（已 clamp 到 [1,total]）
void expand_seq_set(const std::string& seq_set, size_t total,
                    std::vector<std::pair<uint64_t, uint64_t>>& ranges);

// 数值 ID 列表 → "a,b,c"（批量 IN 语句用；全数值，无注入面）
std::string join_mail_ids(const std::vector<uint64_t>& ids);

} // namespace imap_utils
} // namespace mail_system

#endif // MAIL_SYSTEM_IMAP_UTILS_H
