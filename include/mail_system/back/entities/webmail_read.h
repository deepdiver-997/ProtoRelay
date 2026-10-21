#pragma once
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace mail_system::webmail {
struct MailboxSummary {
    uint64_t id = 0;
    std::string name;
    int box_type = 0;   // 1收件箱 2发件箱 3垃圾箱 4草稿箱 5已删除（create_tables_mail2）
    uint64_t total = 0;
    uint64_t unread = 0;
};

struct MailListItem {
    uint64_t id = 0;
    std::string subject;
    std::string sender;      // 本人收件行的发件人（收件箱视角；无则空）
    std::string recipients;  // 本人发件行的收件人 GROUP_CONCAT（发件箱视角）
    std::time_t send_time = 0;
    int status = 0;          // mail_recipients.status 本人收件行：0已读 1未读…
    bool starred = false;
    bool important = false;
    bool deleted = false;
    bool has_attachment = false;
};

struct MailboxListResult {
    bool ok = false;
    bool server_error = false;   // DB 不可用 → 503
    std::vector<MailboxSummary> mailboxes;
};

struct MessagePageResult {
    bool ok = false;
    bool server_error = false;   // DB 不可用 → 503
    bool not_found = false;      // 邮箱不存在 / 非本人 → 404
    MailboxSummary mailbox;
    uint64_t total = 0;
    uint32_t page = 1;
    uint32_t page_size = 0;
    std::vector<MailListItem> messages;
};

struct MailAttachment {
    uint64_t id = 0;
    std::string filename;
    uint64_t size = 0;
    std::string mime_type;
};

// Database-independent metadata. body_key is an opaque provider key, not a
// filesystem path; decoding and storage reads belong to the application service.
struct MessageMetadataResult {
    bool ok = false;
    bool server_error = false;
    bool not_found = false;
    uint64_t id = 0;
    std::string subject;
    std::time_t send_time = 0;
    std::string sender;
    std::vector<std::string> recipients;
    int status = 0;
    bool starred = false;
    bool important = false;
    bool deleted = false;
    std::vector<MailAttachment> attachments;
    std::string body_key;
};
} // namespace mail_system::webmail
