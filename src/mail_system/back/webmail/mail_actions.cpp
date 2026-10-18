#include "mail_system/back/webmail/mail_actions.h"
#include "mail_system/back/common/logger.h"
#include "mail_system/back/common/safe_parse.h"

namespace mail_system::webmail {
namespace {
void run(const std::shared_ptr<pr::ThreadPoolBase>& workers, std::function<void()> task) {
    if (workers) workers->post(std::move(task)); else task();
}
}
void MailActions::update_async(uint64_t user, std::string address, uint64_t box, uint64_t mail,
        std::optional<bool> read, std::optional<bool> starred, std::function<void(MailActionResult)> cb) {
    auto router = router_;
    run(workers_, [router, user, address, box, mail, read, starred, cb = std::move(cb)] {
        std::shared_ptr<pr::DBPool> pool;
        auto conn = ScopedConnection::invalid();
        bool transaction = false;
        auto fail = [&] {
            if (transaction && !(*conn)->rollback()) LOG_AT("WEBAPI", err, "flag rollback failed mail_id={}", mail);
            LOG_AT("WEBAPI", err, "flag update failed user_id={} mail_id={}", user, mail);
            cb({.server_error = true});
        };
        try {
            auto shard = router ? router->route(address) : -1;
            pool = shard < 0 ? nullptr : router->get_db_pool(static_cast<size_t>(shard));
            conn = pool ? pool->acquire_connection() : ScopedConnection::invalid();
            if (!conn->is_valid() || !(*conn)->begin_transaction()) { fail(); return; }
            transaction = true;
            auto row = (*conn)->query(
                "SELECT mb.box_type FROM mail_mailbox mm JOIN mailboxes mb ON mb.id=mm.mailbox_id "
                "WHERE mm.mail_id=? AND mm.mailbox_id=? AND mm.user_id=? AND mb.user_id=? FOR UPDATE",
                {std::to_string(mail), std::to_string(box), std::to_string(user), std::to_string(user)});
            if (!row) { fail(); return; }
            if (!row->get_row_count()) {
                if (!(*conn)->rollback()) { fail(); return; }
                transaction = false; cb({.not_found = true}); return;
            }
            if (read.has_value() && safe_stoull(row->get_value(0, "box_type")) != 2 && !(*conn)->execute(
                "UPDATE mail_recipients SET status=? WHERE mail_id=? AND recipient=? AND status IN (0,1)",
                {*read ? "0" : "1", std::to_string(mail), address})) { fail(); return; }
            if (starred.has_value() && !(*conn)->execute(
                "UPDATE mail_mailbox SET is_starred=? WHERE mail_id=? AND mailbox_id=? AND user_id=?",
                {*starred ? "1" : "0", std::to_string(mail), std::to_string(box), std::to_string(user)})) { fail(); return; }
            if (!(*conn)->commit()) { fail(); return; }
            transaction = false; cb({});
        } catch (const std::exception& e) { LOG_AT("WEBAPI", err, "flag exception: {}", e.what()); fail(); }
    });
}
void MailActions::download_async(uint64_t user, std::string address, uint64_t box, uint64_t mail, uint64_t attachment,
                               std::function<void(AttachmentDownload)> cb) {
    auto router = router_;
    run(workers_, [router, user, address, box, mail, attachment, cb = std::move(cb)] {
        AttachmentDownload out;
        auto fail = [&] { out.server_error = true; LOG_AT("WEBAPI", err, "attachment download failed user_id={} attachment_id={}", user, attachment); cb(std::move(out)); };
        try {
            auto shard = router ? router->route(address) : -1;
            auto pool = shard < 0 ? nullptr : router->get_db_pool(static_cast<size_t>(shard));
            auto conn = pool ? pool->acquire_connection() : ScopedConnection::invalid();
            if (!conn->is_valid()) { fail(); return; }
            auto row = (*conn)->query(
                "SELECT a.filepath,a.filename FROM attachments a JOIN mail_mailbox mm ON mm.mail_id=a.mail_id "
                "JOIN mailboxes mb ON mb.id=mm.mailbox_id "
                "WHERE a.id=? AND a.mail_id=? AND mm.mailbox_id=? AND mm.user_id=? AND mb.user_id=?",
                {std::to_string(attachment), std::to_string(mail), std::to_string(box), std::to_string(user), std::to_string(user)});
            conn.reset();
            if (!row) { fail(); return; }
            if (!row->get_row_count()) { out.not_found = true; cb(std::move(out)); return; }
            auto provider = router->get_storage(static_cast<size_t>(shard));
            if (!provider) { fail(); return; }
            pr::IoError error;
            out.source = provider->open_download(row->get_value(0, "filepath"), error);
            if (!out.source) { LOG_AT("WEBAPI", warn, "download provider error attachment_id={} reason={}", attachment, error.message); fail(); return; }
            out.filename = row->get_value(0, "filename");
            cb(std::move(out));
        } catch (const std::exception& e) { LOG_AT("WEBAPI", err, "download exception: {}", e.what()); fail(); }
    });
}
}
