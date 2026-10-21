#include "mail_system/back/repository/webmail_read_repository.h"
#include "framework/log/log_macros.h"
#include "mail_system/back/common/safe_parse.h"
#include "mail_system/back/db/sql_queries.h"
#include "mail_system/back/router/i_shard_router.h"
#include <algorithm>
#include <utility>

namespace mail_system::repository {
namespace {
using webmail::MailboxListResult;
using webmail::MailboxSummary;
using webmail::MailListItem;
using webmail::MailAttachment;
using webmail::MessagePageResult;
using webmail::MessageMetadataResult;
class SqlWebmailReadRepository final : public IWebmailReadRepository {
public:
    explicit SqlWebmailReadRepository(std::shared_ptr<router::IShardRouter> router)
        : router_(std::move(router)) {}

    void list_mailboxes_async(uint64_t user_id, const std::string& address,
                              std::function<void(MailboxListResult)> cb) override {
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

    // MailService normalizes pagination; only SQL offset saturation lives here.
    void list_messages_async(uint64_t user_id, const std::string& address,
                             uint64_t mailbox_id, uint32_t page, uint32_t page_size,
                             std::function<void(MessagePageResult)> cb) override {
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

    void get_message_metadata_async(uint64_t user_id, const std::string& address,
                           uint64_t mailbox_id, uint64_t mail_id,
                           std::function<void(MessageMetadataResult)> cb) override {
        auto conn = acquire(address);
        if (!conn) { cb({.server_error = true}); return; }

        (*conn)->async_query(db::sql::build_webmail_get_mail_in_mailbox(),
            {std::to_string(mail_id), std::to_string(mailbox_id),
             std::to_string(user_id)},
            [conn, mail_id, cb = std::move(cb)]
            (std::shared_ptr<IDBResult> result) mutable {
                if (!result) { cb({}); return; }
                if (result->get_row_count() == 0) { cb({.not_found = true}); return; }
                auto ctx = std::make_shared<MetadataFetchCtx>();
                ctx->cb = std::move(cb);
                MessageMetadataResult& st = ctx->metadata;
                st.id = safe_stoull(result->get_value(0, "id"));
                st.subject = result->get_value(0, "subject");
                st.send_time =
                    static_cast<std::time_t>(safe_stoull(result->get_value(0, "send_time")));
                st.status = static_cast<int>(safe_stoull(result->get_value(0, "status")));
                st.starred = result->get_value(0, "is_starred") == "1";
                st.important = result->get_value(0, "is_important") == "1";
                st.deleted = result->get_value(0, "is_deleted") == "1";
                ctx->metadata.body_key = result->get_value(0, "body_path");

                // 三条 mail_id 维度的查询顺序链（一条连接同一时刻只承载一个
                // 在途查询——叠放会丢回调，MariaDB 异步驱动同样不支持）
                (*conn)->async_query(db::sql::build_webmail_get_mail_sender(),
                    {std::to_string(mail_id)},
                    [conn, ctx, mail_id]
                    (std::shared_ptr<IDBResult> r) mutable {
                        if (r && r->get_row_count() > 0)
                            ctx->metadata.sender = r->get_value(0, "sender");
                        (*conn)->async_query(db::sql::build_webmail_get_mail_recipients(),
                            {std::to_string(mail_id)},
                            [conn, ctx, mail_id]
                            (std::shared_ptr<IDBResult> r2) mutable {
                                if (r2)
                                    for (size_t i = 0; i < r2->get_row_count(); ++i)
                                        ctx->metadata.recipients.push_back(
                                            r2->get_value(i, "recipient"));
                                (*conn)->async_query(
                                    db::sql::build_webmail_list_mail_attachments(),
                                    {std::to_string(mail_id)},
                                    [conn, ctx]
                                    (std::shared_ptr<IDBResult> r3) mutable {
                                        if (r3)
                                            for (size_t i = 0; i < r3->get_row_count(); ++i) {
                                                MailAttachment a;
                                                a.id = safe_stoull(r3->get_value(i, "id"));
                                                a.filename = r3->get_value(i, "filename");
                                                a.size = safe_stoull(r3->get_value(i, "file_size"));
                                                a.mime_type = r3->get_value(i, "mime_type");
                                                ctx->metadata.attachments.push_back(std::move(a));
                                            }
                                        ctx->metadata.ok = true;
                                        ctx->cb(std::move(ctx->metadata));
                                    });
                            });
                    });
            });
    }

private:
    // ScopedConnection stores a raw pool pointer. Declaration order guarantees
    // that its destructor returns the connection before the owning pool dies.
    struct ReadLease {
        std::shared_ptr<DBPool> pool;
        std::shared_ptr<ScopedConnection> connection;
        IDBConnection* operator->() const { return connection->operator->(); }
    };

    struct MetadataFetchCtx {
        MessageMetadataResult metadata;
        std::function<void(MessageMetadataResult)> cb;
    };

    // 按会话地址路由 shard 并取连接；路由/池/连接任一不可用 → nullptr（503）
    std::shared_ptr<ReadLease> acquire(const std::string& address) const {
        int shard = 0;
        if (router_) { int r = router_->route(address); if (r >= 0) shard = r; }
        auto pool = router_ ? router_->get_db_pool(static_cast<size_t>(shard)) : nullptr;
        if (!pool) { LOG_AT("WEBAPI", err, "mail: no DB pool for shard {}", shard); return nullptr; }
        auto conn = pool->acquire_connection();
        if (!conn || !conn->is_valid()) {
            LOG_AT("WEBAPI", err, "mail: no DB connection for shard {}", shard);
            return nullptr;
        }
        return std::make_shared<ReadLease>(ReadLease{std::move(pool), std::move(conn)});
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
} // namespace

std::shared_ptr<IWebmailReadRepository> make_sql_webmail_read_repository(
    std::shared_ptr<router::IShardRouter> router) {
    return std::make_shared<SqlWebmailReadRepository>(std::move(router));
}
} // namespace mail_system::repository
