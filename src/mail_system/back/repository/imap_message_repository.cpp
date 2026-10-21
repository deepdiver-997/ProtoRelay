#include "mail_system/back/repository/imap_message_repository.h"
#include "mail_system/back/common/safe_parse.h"
#include "mail_system/back/db/sql_queries.h"
#include "mail_system/back/algorithm/snow.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <ctime>
#include <utility>

namespace mail_system::repository {
namespace {
class SqlImapMessageRepository final : public IImapMessageRepository {
public:
    void get_mail_async(Lease lease, uint64_t id, MailComplete complete) override {
        if (!lease || !lease->is_valid()) { if (complete) complete(false, {}); return; }
        (*lease)->async_query("SELECT id, subject, body_path, UNIX_TIMESTAMP(send_time) AS send_time FROM mails WHERE id = ?",
            {std::to_string(id)}, [lease, complete = std::move(complete)](std::shared_ptr<pr::IDBResult> rows) {
                MailboxMailInfo info;
                if (!rows || rows->get_row_count() == 0) { if (complete) complete(false, info); return; }
                info.mail_id = safe_stoull(rows->get_value(0, "id"));
                info.subject = rows->get_value(0, "subject");
                info.body_path = rows->get_value(0, "body_path");
                info.send_time = static_cast<std::time_t>(safe_stoull(rows->get_value(0, "send_time")));
                if (complete) complete(true, std::move(info));
            });
    }
    void get_sender_async(Lease lease, uint64_t id, AddressComplete complete) override {
        if (!lease || !lease->is_valid()) { if (complete) complete({}); return; }
        (*lease)->async_query("SELECT sender FROM mail_recipients WHERE mail_id = ? LIMIT 1",
            {std::to_string(id)}, [lease, complete = std::move(complete)](std::shared_ptr<pr::IDBResult> rows) {
                if (complete) complete(rows && rows->get_row_count() ? rows->get_value(0, "sender") : "");
            });
    }
    void get_recipients_async(Lease lease, uint64_t id, RecipientsComplete complete) override {
        if (!lease || !lease->is_valid()) { if (complete) complete({}); return; }
        (*lease)->async_query("SELECT recipient FROM mail_recipients WHERE mail_id = ?",
            {std::to_string(id)}, [lease, complete = std::move(complete)](std::shared_ptr<pr::IDBResult> rows) {
                std::vector<std::string> recipients;
                if (rows) for (size_t i = 0; i < rows->get_row_count(); ++i) recipients.push_back(rows->get_value(i, "recipient"));
                if (complete) complete(std::move(recipients));
            });
    }
    void get_user_address_async(Lease lease, uint64_t id, AddressComplete complete) override {
        if (!lease || !lease->is_valid()) { if (complete) complete({}); return; }
        (*lease)->async_query("SELECT mail_address FROM users WHERE id = ?",
            {std::to_string(id)}, [lease, complete = std::move(complete)](std::shared_ptr<pr::IDBResult> rows) {
                if (complete) complete(rows && rows->get_row_count() ? rows->get_value(0, "mail_address") : "");
            });
    }
    void insert_mail_async(Lease lease, ImapMailRecord record, Complete complete) override {
        if (!lease || !lease->is_valid()) { if (complete) complete(false); return; }
        (*lease)->async_execute(db::sql::build_imap_append_mail_metadata(),
            {std::to_string(record.mail_id), db::sql::clip_subject_for_db(record.subject.empty() ? "(无主题)" : record.subject),
             record.body_key, std::to_string(record.timestamp)},
            [lease, complete = std::move(complete)](bool ok) { if (complete) complete(ok); });
    }
    void link_mail_async(Lease lease, ImapMailLink link, Complete complete) override {
        if (!lease || !lease->is_valid()) { if (complete) complete(false); return; }
        auto recipient_id = algorithm::get_snowflake_generator().next_id();
        // Parameters are materialized before moving the link into a callback.
        const std::vector<std::string> params{std::to_string(recipient_id), std::to_string(link.mail_id), link.sender,
                                               link.recipient, std::to_string(link.status)};
        (*lease)->async_execute(db::sql::build_imap_append_mail_recipient(), params,
            [lease, link = std::move(link), complete = std::move(complete)](bool ok) mutable {
                if (!ok) { if (complete) complete(false); return; }
                (*lease)->async_execute(db::sql::build_imap_append_mailbox(),
                    {std::to_string(link.mail_id), std::to_string(link.mailbox_id), std::to_string(link.user_id)},
                    [lease, complete = std::move(complete)](bool linked) { if (complete) complete(linked); });
            });
    }
    void remove_mail_async(Lease lease, uint64_t id, Complete complete) override {
        if (!lease || !lease->is_valid()) { if (complete) complete(false); return; }
        (*lease)->async_execute(db::sql::build_delete_mailbox_links_by_mail(), {std::to_string(id)},
            [lease, id, complete = std::move(complete)](bool first) mutable {
                (*lease)->async_execute(db::sql::build_delete_mail_recipients_by_mail(), {std::to_string(id)},
                    [lease, id, first, complete = std::move(complete)](bool second) mutable {
                        (*lease)->async_execute(db::sql::build_delete_mail_by_id(), {std::to_string(id)},
                            [lease, first, second, complete = std::move(complete)](bool third) {
                                if (complete) complete(first && second && third);
                            });
                    });
            });
    }
};
} // namespace
std::shared_ptr<IImapMessageRepository> make_sql_imap_message_repository() { return std::make_shared<SqlImapMessageRepository>(); }
} // namespace mail_system::repository
