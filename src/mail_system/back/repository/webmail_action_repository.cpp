#include "mail_system/back/repository/webmail_action_repository.h"
#include "mail_system/back/common/safe_parse.h"
#include "framework/log/log_macros.h"
#include "mail_system/back/router/i_shard_router.h"
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace mail_system::repository {
namespace {
class SqlWebmailActionRepository final : public IWebmailActionRepository {
public:
    explicit SqlWebmailActionRepository(std::shared_ptr<router::IShardRouter> router)
        : router_(std::move(router)) {}

    webmail::MailActionResult update_flags(const webmail::MailFlagUpdate& update) override {
        const auto& scope = update.scope;
        // Locals destroy the lease before its owning pool, including exceptions.
        std::shared_ptr<pr::DBPool> pool;
        auto connection = pr::ScopedConnection::invalid();
        bool transaction = false;
        auto rollback = [&]() noexcept {
            if (!transaction) return true;
            transaction = false;
            try {
                if ((*connection)->rollback()) return true;
            } catch (...) {}
            // The generic pool does not reset transaction state on return.
            // A failed rollback must never leak a live transaction to a borrower.
            try { (*connection)->disconnect(); } catch (...) {}
            LOG_AT("WEBAPI", err, "flag rollback failed mail_id={}", scope.mail_id);
            return false;
        };
        auto fail = [&] {
            rollback();
            LOG_AT("WEBAPI", err, "flag update failed user_id={} mail_id={}", scope.user_id, scope.mail_id);
            return webmail::MailActionResult{true, false};
        };
        try {
            const int shard = router_ ? router_->route(scope.address) : -1;
            pool = shard < 0 ? nullptr : router_->get_db_pool(static_cast<size_t>(shard));
            connection = pool ? pool->acquire_connection() : pr::ScopedConnection::invalid();
            if (!connection->is_valid() || !(*connection)->begin_transaction()) return fail();
            transaction = true;
            auto row = (*connection)->query(
                "SELECT mb.box_type FROM mail_mailbox mm JOIN mailboxes mb ON mb.id=mm.mailbox_id "
                "WHERE mm.mail_id=? AND mm.mailbox_id=? AND mm.user_id=? AND mb.user_id=? FOR UPDATE",
                {std::to_string(scope.mail_id), std::to_string(scope.mailbox_id),
                 std::to_string(scope.user_id), std::to_string(scope.user_id)});
            if (!row) return fail();
            if (!row->get_row_count()) {
                if (!rollback()) return {true, false};
                return {false, true};
            }
            if (update.read.has_value() && safe_stoull(row->get_value(0, "box_type")) != 2 && !(*connection)->execute(
                "UPDATE mail_recipients SET status=? WHERE mail_id=? AND recipient=? AND status IN (0,1)",
                {*update.read ? "0" : "1", std::to_string(scope.mail_id), scope.address})) return fail();
            if (update.starred.has_value() && !(*connection)->execute(
                "UPDATE mail_mailbox SET is_starred=? WHERE mail_id=? AND mailbox_id=? AND user_id=?",
                {*update.starred ? "1" : "0", std::to_string(scope.mail_id),
                 std::to_string(scope.mailbox_id), std::to_string(scope.user_id)})) return fail();
            if (!(*connection)->commit()) return fail();
            transaction = false;
            return {};
        } catch (const std::exception& error) {
            LOG_AT("WEBAPI", err, "flag exception: {}", error.what());
            return fail();
        }
    }

    webmail::AuthorizedAttachment find_attachment(
        const webmail::MailResourceScope& scope, uint64_t attachment_id) override {
        webmail::AuthorizedAttachment out;
        try {
            const int shard = router_ ? router_->route(scope.address) : -1;
            auto pool = shard < 0 ? nullptr : router_->get_db_pool(static_cast<size_t>(shard));
            auto connection = pool ? pool->acquire_connection() : pr::ScopedConnection::invalid();
            if (!connection->is_valid()) { out.server_error = true; return out; }
            auto row = (*connection)->query(
                "SELECT a.filepath,a.filename FROM attachments a JOIN mail_mailbox mm ON mm.mail_id=a.mail_id "
                "JOIN mailboxes mb ON mb.id=mm.mailbox_id "
                "WHERE a.id=? AND a.mail_id=? AND mm.mailbox_id=? AND mm.user_id=? AND mb.user_id=?",
                {std::to_string(attachment_id), std::to_string(scope.mail_id), std::to_string(scope.mailbox_id),
                 std::to_string(scope.user_id), std::to_string(scope.user_id)});
            if (!row) { out.server_error = true; return out; }
            if (!row->get_row_count()) { out.not_found = true; return out; }
            out.shard = shard;
            out.object_key = row->get_value(0, "filepath");
            out.filename = row->get_value(0, "filename");
            return out;
        } catch (const std::exception& error) {
            LOG_AT("WEBAPI", err, "download metadata exception: {}", error.what());
            out.server_error = true;
            return out;
        }
    }
private:
    std::shared_ptr<router::IShardRouter> router_;
};
} // namespace
std::shared_ptr<IWebmailActionRepository> make_sql_webmail_action_repository(
    std::shared_ptr<router::IShardRouter> router) {
    return std::make_shared<SqlWebmailActionRepository>(std::move(router));
}
} // namespace mail_system::repository
