#pragma once
#include <cstdint>
#include <string>
#include <vector>

// MIME part 预解析结构 — SMTP 收信时解析一次，IMAP 直接遍历
// v3（2026-10-21）：parse_mime_tree 深入 message/rfc822（内嵌消息挂 subs[0]）。
// 旧 sidecar 没有 rfc822 内嵌子树，抬版本号强制从原 MIME 重新解析回写。
inline constexpr uint32_t kMimeMetadataVersion = 3;

struct MimePart {
    std::string type;          // "text", "multipart", "image", etc.
    std::string subtype;       // "plain", "html", "alternative", "mixed", etc.
    std::string charset;       // charset 参数值
    std::string encoding;      // "7bit", "8bit", "base64", "quoted-printable"
    std::string boundary;      // multipart boundary（仅 multipart/* 有效）
    std::string name;          // Content-Disposition filename（附件名）
    std::string disposition;   // attachment / inline，空表示未声明
    std::string content_id;    // Content-ID，不能用附件名替代
    uint32_t metadata_version = 0; // 旧 sidecar 缺失字段时从原 MIME 重新解析
    uint64_t offset = 0;       // 在 body 文件中的起始偏移（含该 part 的 header）
    uint32_t length = 0;       // 在 body 文件中的字节长度
    uint32_t body_size = 0;    // transfer-encoding 状态的正文 octets（BODYSTRUCTURE）
    uint32_t lines = 0;        // body 行数
    std::vector<MimePart> subs; // 子 part（multipart/* 的成员；message/rfc822 的
                                // 内嵌消息根，恒 1 个元素）

    bool is_multipart() const { return type == "multipart"; }
};
