#ifndef MAIL_SYSTEM_WEBMAIL_MIME_VIEW_H
#define MAIL_SYSTEM_WEBMAIL_MIME_VIEW_H

// ================================================================
// mime_view — webmail 读信的 MIME 取材（切片 + transfer 解码 + 部件查找）
//
// 站在 common/mime_parser.h（解析树 + extract_part_content 切片）之上：
// IMAP 按约定交付原始编码内容，webmail 要的是人可读的解码文本，这里补
// quoted-printable / base64 解码，并对解码产物设单 part 上限（正常正文
// 远小于此；超限截断并置 truncated 标志，绝不静默吞）。
// ================================================================

#include "mail_system/back/common/mail_crypto.h"
#include "mail_system/back/common/mime_parser.h"

#include <cstdint>
#include <string>

namespace mail_system::webmail {
namespace mime_view {

// 单个 part 解码后上限。截断的正文仍可读（末尾截断处可能半个字符，可接受）
inline constexpr size_t kMaxDecodedBytes = 1024 * 1024;

struct DecodedBody {
    std::string content;
    bool truncated = false;
};

// ── quoted-printable（RFC 2045 §6.7）────────────────────────────
// =XX 十六进制字节；= 后跟 CRLF/LF 为软换行（删除）；非法 = 序列按字面保留
//（宽松解码：真实邮件里不规范 QP 很常见，比抛错更接近所有主流客户端行为）
inline std::string decode_quoted_printable(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    auto hex_val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '=') { out.push_back(in[i]); continue; }
        if (i + 1 >= in.size()) { out.push_back('='); break; }   // 尾部悬挂 '='
        if (in[i + 1] == '\r' || in[i + 1] == '\n') {            // 软换行
            i += (in[i + 1] == '\r' && i + 2 < in.size() && in[i + 2] == '\n') ? 2 : 1;
            continue;
        }
        if (i + 2 < in.size()) {
            const int hi = hex_val(in[i + 1]), lo = hex_val(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back('=');   // 非 =XX 序列按字面
    }
    return out;
}

// ── 按 part.encoding 解码（切片后的原始内容进来）────────────────
inline DecodedBody decode_part(const std::string& raw_content, const MimePart& part) {
    DecodedBody out;
    std::string enc = part.encoding;
    std::transform(enc.begin(), enc.end(), enc.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (enc == "base64") {
        // base64_decode 不容错空白？——统一先剥掉 WS/换行（真实邮件 base64
        // 按行折行，解码前必须去行缀）
        std::string compact;
        compact.reserve(raw_content.size());
        for (char c : raw_content) {
            if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
            compact.push_back(c);
        }
        out.content = outbound::base64_decode(compact);
    } else if (enc == "quoted-printable") {
        out.content = decode_quoted_printable(raw_content);
    } else {   // 7bit / 8bit / binary / 未知 → 原样
        out.content = raw_content;
    }

    if (out.content.size() > kMaxDecodedBytes) {
        out.content.resize(kMaxDecodedBytes);
        out.truncated = true;
    }
    return out;
}

// ── 深度优先找第一个 text/<subtype> 叶子（multipart 容器本身不是叶子）──
inline const MimePart* find_text_leaf(const MimePart& node,
                                      const std::string& subtype) {
    if (node.type == "text" && node.subtype == subtype) return &node;
    for (const auto& sub : node.subs) {
        if (const MimePart* hit = find_text_leaf(sub, subtype)) return hit;
    }
    return nullptr;
}

} // namespace mime_view
} // namespace mail_system::webmail

#endif // MAIL_SYSTEM_WEBMAIL_MIME_VIEW_H
