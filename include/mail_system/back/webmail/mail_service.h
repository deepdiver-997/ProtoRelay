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

#include "framework/log/log_macros.h"
#include "mail_system/back/common/mime_parser.h"
#include "mail_system/back/common/safe_parse.h"
#include "mail_system/back/db/db_pool.h"
#include "mail_system/back/db/sql_queries.h"
#include "mail_system/back/router/i_shard_router.h"
#include "mail_system/back/webmail/mime_view.h"

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

// 读信：元数据 + 解码后正文（html 未 sanitize——那是 API 层的职责，
// 见 html_sanitize.h）+ 附件清单（下载走后续的鉴权流式端点）。
struct MailAttachment {
    uint64_t id = 0;
    std::string filename;
    uint64_t size = 0;
    std::string mime_type;
};

struct MessageDetail {
    bool ok = false;
    bool server_error = false;   // DB / 存储路由不可用 → 503
    bool not_found = false;      // 无此信 / 不在此箱 / 非本人 → 404
    bool body_available = false; // body 文件缺失/存储故障时 false（信壳照常返回）

    uint64_t id = 0;
    std::string subject;
    std::time_t send_time = 0;
    std::string sender;
    std::vector<std::string> recipients;
    int status = 0;
    bool starred = false;
    bool important = false;
    bool deleted = false;

    bool text_available = false;      // MIME 树里有 text/plain（或全文兜底）
    bool html_available = false;      // MIME 树里有 text/html
    std::string text;                 // 解码后纯文本
    std::string html;                 // 解码后 HTML（未 sanitize）
    std::string charset_text;
    std::string charset_html;
    bool text_truncated = false;
    bool html_truncated = false;

    std::vector<MailAttachment> attachments;
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
                                    for (size_t i = 0; i < result->get_row_count(); ++i) {
                                        auto message = map_mail_row(*result, i);
                                        if (out.mailbox.box_type == 2) message.status = 0;
                                        out.messages.push_back(std::move(message));
                                    }
                                }
                                out.ok = static_cast<bool>(result);
                                cb(std::move(out));
                            });
                    });
            });
    }

    // ── 读信（MVP-2 第二步）─────────────────────────────────────
    // 编排：归属校验（信必须在该箱且属于本人）→ 发件人/收件人/附件三查询
    // 顺序查询 → storage 读 body → MIME 树（sidecar 优先）取材解码。
    // 与读路径契约一致：回调在 storage/DB 事件线程触发，HTTP 写经 exec_ctx_
    // 串行化。body 文件缺失不失败——信壳（元数据/附件表）照常返回，
    // body_available=false 交前端降级展示。
    void get_message_async(uint64_t user_id, const std::string& address,
                           uint64_t mailbox_id, uint64_t mail_id,
                           std::function<void(MessageDetail)> cb) {
        auto conn = acquire(address);
        if (!conn) { cb({.server_error = true}); return; }

        (*conn)->async_query(db::sql::build_webmail_get_mail_in_mailbox(),
            {std::to_string(mail_id), std::to_string(mailbox_id),
             std::to_string(user_id)},
            [this, conn, user_id, address, mailbox_id, mail_id, cb = std::move(cb)]
            (std::shared_ptr<IDBResult> result) mutable {
                if (!result) { cb({}); return; }
                if (result->get_row_count() == 0) { cb({.not_found = true}); return; }
                auto ctx = std::make_shared<MessageFetchCtx>();
                ctx->cb = std::move(cb);
                MessageDetail& st = ctx->detail;
                st.id = safe_stoull(result->get_value(0, "id"));
                st.subject = result->get_value(0, "subject");
                st.send_time =
                    static_cast<std::time_t>(safe_stoull(result->get_value(0, "send_time")));
                st.status = static_cast<int>(safe_stoull(result->get_value(0, "status")));
                st.starred = result->get_value(0, "is_starred") == "1";
                st.important = result->get_value(0, "is_important") == "1";
                st.deleted = result->get_value(0, "is_deleted") == "1";
                const std::string body_path = result->get_value(0, "body_path");

                // 三条 mail_id 维度的查询顺序链（一条连接同一时刻只承载一个
                // 在途查询——叠放会丢回调，MariaDB 异步驱动同样不支持）
                (*conn)->async_query(db::sql::build_webmail_get_mail_sender(),
                    {std::to_string(mail_id)},
                    [this, conn, ctx, address, body_path, mail_id]
                    (std::shared_ptr<IDBResult> r) mutable {
                        if (r && r->get_row_count() > 0)
                            ctx->detail.sender = r->get_value(0, "sender");
                        (*conn)->async_query(db::sql::build_webmail_get_mail_recipients(),
                            {std::to_string(mail_id)},
                            [this, conn, ctx, address, body_path, mail_id]
                            (std::shared_ptr<IDBResult> r2) mutable {
                                if (r2)
                                    for (size_t i = 0; i < r2->get_row_count(); ++i)
                                        ctx->detail.recipients.push_back(
                                            r2->get_value(i, "recipient"));
                                (*conn)->async_query(
                                    db::sql::build_webmail_list_mail_attachments(),
                                    {std::to_string(mail_id)},
                                    [this, conn, ctx, address, body_path]
                                    (std::shared_ptr<IDBResult> r3) mutable {
                                        if (r3)
                                            for (size_t i = 0; i < r3->get_row_count(); ++i) {
                                                MailAttachment a;
                                                a.id = safe_stoull(r3->get_value(i, "id"));
                                                a.filename = r3->get_value(i, "filename");
                                                a.size = safe_stoull(r3->get_value(i, "file_size"));
                                                a.mime_type = r3->get_value(i, "mime_type");
                                                ctx->detail.attachments.push_back(std::move(a));
                                            }
                                        storage_stage(ctx, address, body_path);
                                    });
                            });
                    });
            });
    }

private:
    // 读信请求的可重入上下文：结果 + 完成回调一起走 shared_ptr。
    struct MessageFetchCtx {
        MessageDetail detail;
        std::function<void(MessageDetail)> cb;
    };

    // 读信存储段：body → MIME 树 → 解码正文。provider/文件任一缺失都降级为
    // body_available=false 的成功结果（信壳可用），只有路由层缺失才算故障。
    void storage_stage(std::shared_ptr<MessageFetchCtx> ctx,
                       const std::string& address, const std::string& body_path) {
        int shard = 0;
        if (router_) { int r = router_->route(address); if (r >= 0) shard = r; }
        auto provider = router_ ? router_->get_storage(static_cast<size_t>(shard)) : nullptr;
        if (!provider || body_path.empty()) {
            LOG_AT("WEBAPI", warn, "message {}: no storage provider or empty body_path",
                   ctx->detail.id);
            ctx->detail.ok = true;
            ctx->cb(std::move(ctx->detail));
            return;
        }
        provider->async_read_all(body_path,
            [this, ctx, body_path, provider](bool ok, std::string raw, const pr::storage::IoError& error) mutable {
                if (!ok || raw.empty()) {
                    LOG_AT("WEBAPI", warn, "message {}: body read failed ({})",
                           ctx->detail.id, error.message);
                    ctx->detail.ok = true;
                    ctx->cb(std::move(ctx->detail));
                    return;
                }
                ctx->detail.body_available = true;
                fill_body(ctx->detail, body_path, raw, provider);
                ctx->detail.ok = true;
                ctx->cb(std::move(ctx->detail));
            });
    }

    // MIME 取材：sidecar 优先（miss 则现场解析并回写，与 IMAP 同一份缓存）。
    // 纯文本兜底：无 MIME 树的旧邮件按"顶层 header 后全文"处理。
    static void fill_body(MessageDetail& st, const std::string& body_path,
                          const std::string& raw,
                          const std::shared_ptr<pr::storage::IStorageProvider>& provider) {
        MimePart tree;
        auto metadata = mime_metadata_store_for(provider);
        if (!ensure_mime_tree(metadata, body_path, raw, tree)) {
            size_t sep = raw.find("\r\n\r\n");
            if (sep == std::string::npos) sep = raw.find("\n\n");
            if (sep != std::string::npos) {
                st.text = raw.substr(sep + (raw[sep] == '\r' ? 4 : 2));
                if (st.text.size() > mime_view::kMaxDecodedBytes) {
                    st.text.resize(mime_view::kMaxDecodedBytes);
                    st.text_truncated = true;
                }
                st.text_available = true;
            }
            return;
        }
        if (const auto* p = mime_view::find_text_leaf(tree, "plain")) {
            auto d = mime_view::decode_part(extract_part_content(raw, *p), *p);
            st.text = std::move(d.content);
            st.text_truncated = d.truncated;
            st.charset_text = p->charset;
            st.text_available = true;
        }
        if (const auto* p = mime_view::find_text_leaf(tree, "html")) {
            auto d = mime_view::decode_part(extract_part_content(raw, *p), *p);
            st.html = std::move(d.content);
            st.html_truncated = d.truncated;
            st.charset_html = p->charset;
            st.html_available = true;
        }
    }

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
