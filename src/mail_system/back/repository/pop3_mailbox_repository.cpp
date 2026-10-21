#include "mail_system/back/repository/pop3_mailbox_repository.h"
#include "mail_system/back/common/safe_parse.h"
#include "mail_system/back/db/sql_queries.h"
#include "pop3_repository_completion.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mail_system::repository {
namespace {
class SqlPop3MailboxRepository final : public IPop3MailboxRepository {
public:
    void get_inbox_async(Pop3DbContext context, uint64_t user, InboxComplete complete) override {
        auto done = std::make_shared<detail::Pop3Completion<bool, uint64_t>>(std::move(complete));
        if (!context || !context->valid()) { done->finish(false, 0); return; }
        try {
            (*context->lease)->async_query(db::sql::build_imap_get_inbox_id(), {std::to_string(user)},
                [context, done](std::shared_ptr<pr::IDBResult> rows) {
                    bool ok = false; uint64_t id = 0;
                    try { if (rows) { id = rows->get_row_count() ? safe_stoull(rows->get_value(0, "id")) : 0; ok = true; } }
                    catch (...) { ok = false; }
                    done->finish(ok, id);
                });
        } catch (...) { done->finish(false, 0); }
    }
    void get_messages_async(Pop3DbContext context, uint64_t user, uint64_t mailbox, MessagesComplete complete) override {
        auto done = std::make_shared<detail::Pop3Completion<bool, std::vector<MailboxMailInfo>>>(std::move(complete));
        if (!context || !context->valid()) { done->finish(false, {}); return; }
        try {
            // Same rows/order as IMailboxRepository; POP3 only needs its message
            // identity/body subset. Keep mapping guarded inside this boundary.
            (*context->lease)->async_query(db::sql::build_imap_get_mailbox_mails(),
                {std::to_string(mailbox), std::to_string(user)},
                [context, done](std::shared_ptr<pr::IDBResult> rows) {
                    bool ok = false; std::vector<MailboxMailInfo> messages;
                    try {
                        if (rows) {
                            for (size_t i = 0; i < rows->get_row_count(); ++i) {
                                MailboxMailInfo mail;
                                mail.mail_id = safe_stoull(rows->get_value(i, "id"));
                                mail.body_path = rows->get_value(i, "body_path");
                                mail.is_deleted = rows->get_value(i, "is_deleted") == "1";
                                messages.push_back(std::move(mail));
                            }
                            ok = true;
                        }
                    } catch (...) { messages.clear(); }
                    done->finish(ok, std::move(messages));
                });
        } catch (...) { done->finish(false, {}); }
    }
    void apply_deletions_async(Pop3DbContext context, Pop3DeletionRequest request, DeleteComplete complete) override {
        auto done = std::make_shared<detail::Pop3Completion<Pop3DeletionResult>>(std::move(complete));
        if (request.mail_ids.empty()) { done->finish({true, false}); return; }
        if (!context || !context->valid()) { done->finish({}); return; }
        try {
            std::string ids;
            for (auto id : request.mail_ids) { if (!ids.empty()) ids += ','; ids += std::to_string(id); }
            const auto sql = "UPDATE mail_mailbox SET is_deleted = 1 WHERE mail_id IN (" + ids + ")"
                " AND user_id = " + std::to_string(request.user_id) + " AND mailbox_id = " + std::to_string(request.mailbox_id);
            (*context->lease)->async_execute(sql,
                [context, request = std::move(request), done](bool marked) {
                    if (!marked) { done->finish({false, true}); return; }
                    try {
                        (*context->lease)->async_execute(db::sql::build_imap_expunge_delete_mailbox(),
                            {std::to_string(request.mailbox_id), std::to_string(request.user_id)},
                            [context, done](bool removed) { done->finish({removed, true}); });
                    } catch (...) { done->finish({false, true}); }
                });
        } catch (...) { done->finish({false, true}); }
    }
};
} // namespace
std::shared_ptr<IPop3MailboxRepository> make_sql_pop3_mailbox_repository() { return std::make_shared<SqlPop3MailboxRepository>(); }
} // namespace mail_system::repository
