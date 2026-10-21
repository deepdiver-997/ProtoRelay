#include "mail_system/back/repository/mailbox_repository.h"
#include "mail_system/back/db/sql_queries.h"
#include "mail_system/back/common/safe_parse.h"
#include <algorithm>
#include <cctype>

namespace mail_system::repository {
namespace {
class SqlMailboxRepository final : public IMailboxRepository {
public:
    void get_mailboxes_async(Lease, uint64_t, ListCallback) override;
    void find_mailbox_id_async(Lease, uint64_t, const std::string&, std::function<void(uint64_t)>) override;
    void get_mailbox_mails_async(Lease, uint64_t, uint64_t, MessagesCallback) override;
    void get_mailbox_count_async(Lease, uint64_t, uint64_t, std::function<void(size_t)>) override;
    void get_mailbox_unseen_count_async(Lease, uint64_t, uint64_t, std::function<void(size_t)>) override;
    void get_mailbox_uidnext_async(Lease, uint64_t, uint64_t, std::function<void(uint64_t)>) override;
};
void SqlMailboxRepository::get_mailboxes_async(
    std::shared_ptr<pr::ScopedConnection> conn, uint64_t user_id,
    std::function<void(bool, std::vector<MailboxSummary>)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(false, {}); return; }
    (*conn)->async_query(db::sql::build_imap_list_mailboxes(), {std::to_string(user_id)},
        [conn, cb = std::move(cb)](std::shared_ptr<pr::IDBResult> result) mutable {
            std::vector<MailboxSummary> mailboxes;
            if (!result) { cb(false, std::move(mailboxes)); return; }
            for (size_t i = 0; i < result->get_row_count(); ++i) {
                uint64_t id = safe_stoull(result->get_value(i, "id"));
                std::string name = result->get_value(i, "name");
                int box_type = static_cast<int>(safe_stoull(result->get_value(i, "box_type")));
                mailboxes.push_back({id, std::move(name), box_type});
            }
            cb(true, std::move(mailboxes));
        });
}

void SqlMailboxRepository::find_mailbox_id_async(
    std::shared_ptr<pr::ScopedConnection> conn, uint64_t user_id,
    const std::string& mailbox_name, std::function<void(uint64_t)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(0); return; }
    std::string name_utf8 = mailbox_name;
    (*conn)->async_query(db::sql::build_imap_get_mailbox_by_name(),
        {std::to_string(user_id), name_utf8},
        [conn, user_id, name_utf8, cb = std::move(cb)](std::shared_ptr<pr::IDBResult> result) mutable {
            if (result && result->get_row_count() > 0) {
                cb(safe_stoull(result->get_value(0, "id")));
                return;
            }
            // INBOX 兜底（box_type=1，中文名邮箱可能直接查不到）
            std::string upper = name_utf8;
            std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            if (upper != "INBOX") { cb(0); return; }
            (*conn)->async_query(db::sql::build_imap_get_inbox_id(),
                {std::to_string(user_id)},
                [conn, cb = std::move(cb)](std::shared_ptr<pr::IDBResult> r2) mutable {
                    if (r2 && r2->get_row_count() > 0) cb(safe_stoull(r2->get_value(0, "id")));
                    else cb(0);
                });
        });
}

void SqlMailboxRepository::get_mailbox_mails_async(
    std::shared_ptr<pr::ScopedConnection> conn, uint64_t mailbox_id, uint64_t user_id,
    std::function<void(bool, std::vector<MailboxMailInfo>)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(false, {}); return; }
    (*conn)->async_query(db::sql::build_imap_get_mailbox_mails(),
        {std::to_string(mailbox_id), std::to_string(user_id)},
        [conn, cb = std::move(cb)](std::shared_ptr<pr::IDBResult> result) mutable {
            std::vector<MailboxMailInfo> mails;
            if (!result) { cb(false, std::move(mails)); return; }
            for (size_t i = 0; i < result->get_row_count(); ++i) {
                MailboxMailInfo info;
                info.mail_id = safe_stoull(result->get_value(i, "id"));
                info.sender = result->get_value(i, "sender");
                info.recipient = result->get_value(i, "recipient");
                info.subject = result->get_value(i, "subject");
                info.body_path = result->get_value(i, "body_path");
                info.is_starred = result->get_value(i, "is_starred") == "1";
                info.is_deleted = result->get_value(i, "is_deleted") == "1";
                info.is_important = result->get_value(i, "is_important") == "1";
                info.status = result->get_value(i, "status").empty() ? 0 : static_cast<int>(safe_stoull(result->get_value(i, "status")));
                info.send_time = static_cast<time_t>(safe_stoull(result->get_value(i, "send_time")));
                mails.push_back(std::move(info));
            }
            cb(true, std::move(mails));
        });
}

void SqlMailboxRepository::get_mailbox_count_async(
    std::shared_ptr<pr::ScopedConnection> conn, uint64_t mailbox_id, uint64_t user_id,
    std::function<void(size_t)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(0); return; }
    (*conn)->async_query(db::sql::build_imap_select_status_total(),
        {std::to_string(mailbox_id), std::to_string(user_id)},
        [conn, cb = std::move(cb)](std::shared_ptr<pr::IDBResult> result) mutable {
            if (result && result->get_row_count() > 0)
                cb(static_cast<size_t>(safe_stoull(result->get_value(0, "cnt"))));
            else cb(0);
        });
}

void SqlMailboxRepository::get_mailbox_unseen_count_async(
    std::shared_ptr<pr::ScopedConnection> conn, uint64_t mailbox_id, uint64_t user_id,
    std::function<void(size_t)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(0); return; }
    (*conn)->async_query(db::sql::build_imap_mailbox_unseen_count(),
        {std::to_string(user_id), std::to_string(mailbox_id), std::to_string(user_id)},
        [conn, cb = std::move(cb)](std::shared_ptr<pr::IDBResult> result) mutable {
            if (result && result->get_row_count() > 0)
                cb(static_cast<size_t>(safe_stoull(result->get_value(0, "cnt"))));
            else cb(0);
        });
}

void SqlMailboxRepository::get_mailbox_uidnext_async(
    std::shared_ptr<pr::ScopedConnection> conn, uint64_t mailbox_id, uint64_t user_id,
    std::function<void(uint64_t)> cb)
{
    (void)user_id;   // mailbox_id 是 mailboxes 主键，全局唯一，无需 user_id 过滤
    if (!conn || !conn->is_valid()) { if (cb) cb(1); return; }
    // 原子推进高水位：行锁串行化并发/跨实例读者，GREATEST 防 expunge 后回退；
    // 推进后再读取（advance 是写，read 才是值）
    (*conn)->async_execute(db::sql::build_imap_uidnext_advance(),
        {std::to_string(mailbox_id), std::to_string(mailbox_id), std::to_string(mailbox_id)},
        [conn, mailbox_id, cb = std::move(cb)](bool) mutable {
            (*conn)->async_query(db::sql::build_imap_uidnext_read(),
                {std::to_string(mailbox_id)},
                // ⚠ 持 conn：外层回调返回后链上只剩此捕获撑住 ScopedConnection，
                // 漏捕 = advance 查询完成瞬间连接带飞归池（2026-09-24 crash 根因）
                [conn, cb = std::move(cb)](std::shared_ptr<pr::IDBResult> result) mutable {
                    if (result && result->get_row_count() > 0)
                        cb(safe_stoull(result->get_value(0, "uidnext")));
                    else cb(1);
                });
        });
}

} // namespace
std::shared_ptr<IMailboxRepository> make_sql_mailbox_repository() {
    return std::make_shared<SqlMailboxRepository>();
}
} // namespace mail_system::repository
