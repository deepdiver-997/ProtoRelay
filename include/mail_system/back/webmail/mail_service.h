#ifndef MAIL_SYSTEM_WEBMAIL_MAIL_SERVICE_H
#define MAIL_SYSTEM_WEBMAIL_MAIL_SERVICE_H

// ================================================================
// MailService — webmail 读路径编排（kickoff MVP-2：列表/分页）
//
// mailboxes / mail_mailbox / mails 三表只读（kickoff 设计决策 3），
// 分片路由按会话地址定位 shard（用户与其邮件同 shard）。全部走
// async_query：io 线程发起，回调在 DB 事件线程触发——HTTP 写经 exec_ctx_
// 串行化，跨线程安全（与登录端点同一形状，见 api_router.h 认领契约）。
//
// 失败语义与登录同口径：DB 不可用 → server_error（上层 503 fail-closed）；
// 邮箱不存在/不属于本人 → not_found（上层 404，不区分"无此箱/非你的箱"，
// 不给跨用户探测通道）。
// ================================================================

#include "mail_system/back/common/safe_parse.h"
#include "mail_system/back/db/db_pool.h"
#include "mail_system/back/db/sql_queries.h"
#include "mail_system/back/router/i_shard_router.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
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

class MailService {
public:
    static constexpr uint32_t kMaxPageSize = 100;

    explicit MailService(std::shared_ptr<router::IShardRouter> shard_router)
        : router_(std::move(shard_router)) {}

    void list_mailboxes_async(uint64_t user_id, const std::string& address,
                              std::function<void(MailboxListResult)> cb) {
        auto conn = acquire(address);
        if (!conn) { cb({.ok = false, .server_error = true}); return; }
        (*conn)->async_query(db::sql::build_webmail_list_mailboxes(),
            {address, std::to_string(user_id)},
            [conn, cb = std::move(cb)](std::shared_ptr<IDBResult> result) mutable {
                MailboxListResult out;
                if (!result) { cb(std::move(out)); return; }
                for (size_t i = 0; i < result->get_row_count(); ++i) {
                    MailboxSummary mb;
                    mb.id = safe_stoull(result->get_value(i, "id"));
                    mb.name = result->get_value(i, "name");
                    mb.box_type = static_cast<int>(safe_stoull(result->get_value(i, "box_type")));
                    mb.total = safe_stoull(result->get_value(i, "total"));
                    mb.unread = safe_stoull(result->get_value(i, "unread"));
                    out.mailboxes.push_back(std::move(mb));
                }
                out.ok = true;
                cb(std::move(out));
            });
    }

    // page / page_size 已由 API 层严格 parse；此处再钳制一次（单点防御，
    // page_size > 100 钳到 100，page 下限 1），调用方传什么都产出合法分页。
    void list_messages_async(uint64_t user_id, const std::string& address,
                             uint64_t mailbox_id, uint32_t page, uint32_t page_size,
                             std::function<void(MessagePageResult)> cb) {
        if (page_size == 0) page_size = 1;               // API 层已 400，此处纯防御
        if (page_size > kMaxPageSize) page_size = kMaxPageSize;
        if (page == 0) page = 1;
        const uint64_t offset = static_cast<uint64_t>(page - 1) * page_size;

        auto conn = acquire(address);
        if (!conn) { cb({.server_error = true}); return; }

        (*conn)->async_query(db::sql::build_webmail_get_mailbox_by_id(),
            {std::to_string(mailbox_id), std::to_string(user_id)},
            [conn, user_id, address, mailbox_id, page, page_size, offset,
             cb = std::move(cb)](std::shared_ptr<IDBResult> result) mutable {
                MessagePageResult out;
                out.page = page;
                out.page_size = page_size;
                if (!result) { cb(std::move(out)); return; }
                if (result->get_row_count() == 0) { out.not_found = true; cb(std::move(out)); return; }
                out.mailbox.id = safe_stoull(result->get_value(0, "id"));
                out.mailbox.name = result->get_value(0, "name");
                out.mailbox.box_type = static_cast<int>(safe_stoull(result->get_value(0, "box_type")));

                (*conn)->async_query(db::sql::build_webmail_count_mailbox_mails(),
                    {std::to_string(mailbox_id), std::to_string(user_id)},
                    [conn, user_id, address, mailbox_id, page, page_size, offset,
                     mailbox = out.mailbox, cb = std::move(cb)]
                    (std::shared_ptr<IDBResult> result) mutable {
                        MessagePageResult out;
                        out.mailbox = std::move(mailbox);
                        out.page = page;
                        out.page_size = page_size;
                        if (!result) { cb(std::move(out)); return; }
                        out.total = safe_stoull(result->get_value(0, "total"));

                        if (out.total == 0) { out.ok = true; cb(std::move(out)); return; }
                        (*conn)->async_query(
                            db::sql::build_webmail_list_mailbox_mails(page_size,
                                static_cast<uint32_t>(std::min<uint64_t>(
                                    offset, UINT32_MAX))),
                            {address, address, address,
                             std::to_string(mailbox_id), std::to_string(user_id)},
                            [conn, out, cb = std::move(cb)]
                            (std::shared_ptr<IDBResult> result) mutable {
                                if (result) {
                                    for (size_t i = 0; i < result->get_row_count(); ++i)
                                        out.messages.push_back(map_mail_row(*result, i));
                                }
                                out.ok = static_cast<bool>(result);
                                cb(std::move(out));
                            });
                    });
            });
    }

private:
    // 按会话地址路由 shard 并取连接；路由/池/连接任一不可用 → nullptr（503）
    std::shared_ptr<ScopedConnection> acquire(const std::string& address) const {
        int shard = 0;
        if (router_) { int r = router_->route(address); if (r >= 0) shard = r; }
        auto pool = router_ ? router_->get_db_pool(static_cast<size_t>(shard)) : nullptr;
        if (!pool) { LOG_AT("WEBAPI", err, "mail: no DB pool for shard {}", shard); return nullptr; }
        auto conn = pool->acquire_connection();
        if (!conn || !conn->is_valid()) {
            LOG_AT("WEBAPI", err, "mail: no DB connection for shard {}", shard);
            return nullptr;
        }
        return conn;
    }

    static MailListItem map_mail_row(const IDBResult& result, size_t i) {
        MailListItem item;
        item.id = safe_stoull(result.get_value(i, "id"));
        item.subject = result.get_value(i, "subject");
        item.sender = result.get_value(i, "sender");
        item.recipients = result.get_value(i, "recipients");
        item.send_time = static_cast<std::time_t>(safe_stoull(result.get_value(i, "send_time")));
        item.status = static_cast<int>(safe_stoull(result.get_value(i, "status")));
        item.starred = result.get_value(i, "is_starred") == "1";
        item.important = result.get_value(i, "is_important") == "1";
        item.deleted = result.get_value(i, "is_deleted") == "1";
        item.has_attachment = result.get_value(i, "has_attachment") == "1";
        return item;
    }

    std::shared_ptr<router::IShardRouter> router_;
};

} // namespace mail_system::webmail

#endif // MAIL_SYSTEM_WEBMAIL_MAIL_SERVICE_H
