#include "mail_system/back/repository/imap_mutation_repository.h"
#include "mail_system/back/db/sql_queries.h"
#include <charconv>
#include <cstdint>
#include <memory>
#include <limits>
#include <system_error>
#include <string>
#include <vector>
#include <utility>

namespace mail_system::repository {
namespace {
using Lease = IImapMutationRepository::Lease;

std::string join_ids(const std::vector<uint64_t>& ids) {
    std::string result;
    for (auto id : ids) {
        if (!result.empty()) result += ',';
        result += std::to_string(id);
    }
    return result;
}

// An operation is kept alive only by the currently pending DB callback. There
// is no self-owning recursive function cycle after inline or delayed completion.
struct StoreOperation : std::enable_shared_from_this<StoreOperation> {
    Lease connection;
    ImapStoreRequest request;
    IImapMutationRepository::Complete complete;
    ImapMutationResult result{true};
    std::string recipient;

    void start() {
        auto self = shared_from_this();
        (*connection)->async_query("SELECT mail_address FROM users WHERE id = ?",
            {std::to_string(request.scope.user_id)},
            [self](std::shared_ptr<pr::IDBResult> rows) {
                if (rows && rows->get_row_count()) self->recipient = rows->get_value(0, "mail_address");
                // The established STORE path falls back to the authenticated
                // address if the lookup is absent or unavailable.
                if (self->recipient.empty()) self->recipient = self->request.recipient_fallback;
                self->next(0);
            });
    }
    void next(unsigned step) {
        if (step >= 3) {
            auto callback = std::move(complete);
            if (callback) callback(result);
            return;
        }
        const auto& flag = step == 0 ? request.seen : step == 1 ? request.flagged : request.deleted;
        if (!flag.has_value()) { next(step + 1); return; }
        std::string sql;
        std::vector<std::string> params;
        if (step == 0) {
            sql = "UPDATE mail_recipients SET status = ? WHERE mail_id IN (" + join_ids(request.mail_ids)
                + ") AND recipient = ?";
            params = {*flag ? "0" : "1", recipient};
        } else {
            sql = std::string("UPDATE mail_mailbox SET ") + (step == 1 ? "is_starred" : "is_deleted")
                + " = ? WHERE mail_id IN (" + join_ids(request.mail_ids)
                + ") AND user_id = ? AND mailbox_id = ?";
            params = {*flag ? "1" : "0", std::to_string(request.scope.user_id),
                      std::to_string(request.scope.mailbox_id)};
        }
        auto self = shared_from_this();
        (*connection)->async_execute(sql, params, [self, step](bool ok) {
            self->result.ok = self->result.ok && ok;
            // STORE historically attempts each requested attribute even when
            // an earlier write fails. The result reports the aggregate status.
            self->next(step + 1);
        });
    }
};

struct CopyMoveOperation : std::enable_shared_from_this<CopyMoveOperation> {
    Lease connection;
    ImapCopyMoveRequest request;
    IImapMutationRepository::CopyComplete complete;
    ImapCopyMoveResult result;

    void start() {
        std::string values;
        for (auto id : request.mail_ids) {
            if (!values.empty()) values += ',';
            values += '(' + std::to_string(id) + ',' + std::to_string(request.target_mailbox_id)
                + ',' + std::to_string(request.source.user_id) + ",0,0,0)";
        }
        auto self = shared_from_this();
        (*connection)->async_execute(
            "INSERT IGNORE INTO mail_mailbox (mail_id, mailbox_id, user_id, is_starred, is_important, is_deleted) VALUES " + values,
            [self](bool ok) {
                if (!ok) { self->after_copy(); return; }
                // ROW_COUNT belongs to this INSERT and must use this same
                // retained connection before any subsequent mutation.
                (*self->connection)->async_query(db::sql::build_select_row_count(),
                    [self](std::shared_ptr<pr::IDBResult> rows) {
                        if (rows && rows->get_row_count() > 0) {
                            const auto count = rows->get_value(0, "affected");
                            uint64_t value = 0;
                            const auto parsed = std::from_chars(count.data(), count.data() + count.size(), value);
                            self->result.copy_ok = parsed.ec == std::errc{} && parsed.ptr == count.data() + count.size()
                                && value <= std::numeric_limits<size_t>::max();
                            if (self->result.copy_ok) self->result.copied = static_cast<size_t>(value);
                        }
                        self->after_copy();
                    });
            });
    }
    void after_copy() {
        if (!request.move || !result.copy_ok) { finish(); return; }
        // Never delete the source after a failed INSERT/count. A successful
        // count of zero is valid (all target links already existed).
        auto self = shared_from_this();
        (*connection)->async_execute(
            "UPDATE mail_mailbox SET is_deleted = 1 WHERE mail_id IN (" + join_ids(request.mail_ids)
                + ") AND user_id = ? AND mailbox_id = ?",
            {std::to_string(request.source.user_id), std::to_string(request.source.mailbox_id)},
            [self](bool ok) { self->result.source_marked = ok; self->finish(); });
    }
    void finish() {
        auto callback = std::move(complete);
        if (callback) callback(result);
    }
};

class SqlImapMutationRepository final : public IImapMutationRepository {
public:
    void store_async(Lease connection, ImapStoreRequest request, Complete complete) override {
        if (!connection || !connection->is_valid()) { if (complete) complete({false}); return; }
        if (request.mail_ids.empty()) { if (complete) complete({true}); return; }
        auto operation = std::make_shared<StoreOperation>();
        operation->connection = std::move(connection);
        operation->request = std::move(request);
        operation->complete = std::move(complete);
        operation->start();
    }
    void expunge_async(Lease connection, ImapMailboxScope scope, Complete complete) override {
        if (!connection || !connection->is_valid()) { if (complete) complete({false}); return; }
        (*connection)->async_execute(db::sql::build_imap_expunge_delete_mailbox(),
            {std::to_string(scope.mailbox_id), std::to_string(scope.user_id)},
            [connection, complete = std::move(complete)](bool ok) mutable {
                if (complete) complete({ok});
            });
    }
    void copy_move_async(Lease connection, ImapCopyMoveRequest request, CopyComplete complete) override {
        if (!connection || !connection->is_valid()) { if (complete) complete({}); return; }
        // A same-mailbox MOVE has no independent target link to preserve;
        // marking the source would delete the only mailbox membership.
        if (request.move && request.source.mailbox_id == request.target_mailbox_id) {
            if (complete) complete({});
            return;
        }
        if (request.mail_ids.empty()) { if (complete) complete({true, 0, false}); return; }
        auto operation = std::make_shared<CopyMoveOperation>();
        operation->connection = std::move(connection);
        operation->request = std::move(request);
        operation->complete = std::move(complete);
        operation->start();
    }
};
} // namespace
std::shared_ptr<IImapMutationRepository> make_sql_imap_mutation_repository() {
    return std::make_shared<SqlImapMutationRepository>();
}
} // namespace mail_system::repository
