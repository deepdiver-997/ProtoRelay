#include "mail_system/back/repository/persistence_repository.h"
#include "mail_system/back/common/safe_parse.h"
#include "mail_system/back/db/sql_queries.h"
#include "mail_system/back/router/i_shard_router.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>

namespace mail_system::repository {
namespace {
std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
std::string domain(const std::string& address) {
    auto at = address.find('@');
    return at == std::string::npos ? std::string{} : lower(address.substr(at + 1));
}
int source_shard(const std::shared_ptr<router::IShardRouter>& router, const ::mail& message) {
    const auto& key = !message.from.empty() ? message.from : !message.to.empty() ? message.to.front() : std::string{};
    const auto shard = router && !key.empty() ? router->route(key) : 0;
    return shard < 0 ? 0 : shard;
}
struct DatabaseOwner {
    std::shared_ptr<pr::DBPool> pool;
    std::shared_ptr<pr::ScopedConnection> lease;
    explicit DatabaseOwner(std::shared_ptr<pr::DBPool> value) : pool(std::move(value)) {
        if (pool) lease = pool->acquire_connection();
    }
    bool valid() const { return lease && lease->is_valid(); }
    pr::IDBConnection* connection() const { return lease ? lease->operator->() : nullptr; }
};
void disconnect(const std::shared_ptr<DatabaseOwner>& database) noexcept {
    try { if (database && database->connection()) database->connection()->disconnect(); } catch (...) {}
}
struct PrimaryOperation : std::enable_shared_from_this<PrimaryOperation> {
    std::shared_ptr<router::IShardRouter> router;
    PersistenceRequest request;
    IPersistenceRepository::Complete complete;
    std::shared_ptr<DatabaseOwner> database;
    PersistenceResult result;
    std::atomic<bool> done{false}, rolling_back{false};
    bool transaction = false;
    size_t dedup_index = 0, mailbox_index = 0, outbox_index = 0;
    pr::IDBConnection* connection() const { return database->connection(); }
    template <typename Task> void guard(Task task) {
        if (done.load() || rolling_back.load()) return;
        try { task(); }
        catch (const std::exception& error) { fail(error.what()); }
        catch (...) { fail("Persistence continuation failed"); }
    }
    void finish(PersistenceDisposition disposition, std::string error = {}) {
        if (done.exchange(true)) return;
        result.disposition = disposition; result.error = std::move(error);
        if (disposition != PersistenceDisposition::committed) result.outbox_records.clear();
        auto callback = std::move(complete);
        if (callback) callback(std::move(result));
    }
    void fail(std::string error) {
        if (done.load() || rolling_back.exchange(true)) return;
        if (!transaction) { finish(PersistenceDisposition::failed, std::move(error)); return; }
        transaction = false;
        auto self = shared_from_this();
        auto rollback = [self, error = std::move(error)](bool ok) mutable {
            if (self->done.load()) return;
            if (!ok) disconnect(self->database);
            self->result.transaction = ok ? PersistenceTransaction::rolled_back : PersistenceTransaction::rollback_failed;
            self->finish(PersistenceDisposition::failed, std::move(error));
        };
        try { connection()->async_rollback(rollback); }
        catch (...) { rollback(false); }
    }
    void start() {
        auto self = shared_from_this();
        guard([self] {
            if (!self->request.message) { self->fail("Mail data is null"); return; }
            self->result.source_shard = source_shard(self->router, *self->request.message);
            auto pool = self->router ? self->router->get_db_pool(static_cast<size_t>(self->result.source_shard)) : nullptr;
            if (!pool) { self->finish(PersistenceDisposition::file_only); return; }
            self->database = std::make_shared<DatabaseOwner>(std::move(pool));
            if (!self->database->valid()) { self->fail("Failed to get database connection"); return; }
            self->deduplicate_next();
        });
    }
    void deduplicate_next() {
        const auto& message = *request.message;
        if (message.source_message_id.empty() || message.to.empty()) { begin(); return; }
        if (dedup_index == message.to.size()) { finish(PersistenceDisposition::deduplicated); return; }
        const auto sql = db::sql::build_dedup_by_message_id(message.from, message.to[dedup_index++], message.source_message_id, connection());
        auto self = shared_from_this();
        connection()->async_query(sql, [self](std::shared_ptr<pr::IDBResult> rows) {
            self->guard([self, rows] {
                // Preserve established Message-ID semantics: a null/empty lookup
                // is a miss; no subject/Bloom fallback is activated here.
                if (!rows || !rows->get_row_count()) self->begin();
                else self->deduplicate_next();
            });
        });
    }
    void begin() {
        auto self = shared_from_this();
        transaction = true; // A throwing begin has an uncertain transaction state.
        connection()->async_begin_transaction([self](bool ok) {
            self->guard([self, ok] {
                if (!ok) { self->transaction = false; self->fail("Failed to begin transaction"); return; }
                self->insert_metadata();
            });
        });
    }
    void insert_metadata() {
        const auto& message = *request.message;
        if (message.to.empty() || message.ids.size() < message.to.size()) { fail("Invalid mail recipient metadata"); return; }
        const auto sql = db::sql::build_insert_mail(message.id, message.subject, message.body_path, message.send_time, connection());
        auto self = shared_from_this();
        connection()->async_execute(sql, [self](bool ok) {
            self->guard([self, ok] {
                if (!ok) { self->fail("Failed to insert mail metadata"); return; }
                const auto sql = db::sql::build_insert_recipients(*self->request.message, self->request.options.local_domain, self->connection());
                self->connection()->async_execute(sql, [self](bool ok) {
                    self->guard([self, ok] { if (!ok) self->fail("Failed to insert mail recipients"); else self->sent_copy(); });
                });
            });
        });
    }
    void sent_copy() {
        if (!request.message->save_sent_copy) { local_mailboxes_next(); return; }
        auto self = shared_from_this();
        connection()->async_query(
            "SELECT mb.id AS mailbox_id,mb.user_id FROM mailboxes mb JOIN users u ON u.id=mb.user_id "
            "WHERE u.mail_address=? AND u.status=1 AND mb.box_type=2 ORDER BY mb.id LIMIT 1", {request.message->from},
            [self](std::shared_ptr<pr::IDBResult> boxes) {
                self->guard([self, boxes] {
                    if (!boxes || !boxes->get_row_count()) { self->fail("Sent mailbox unavailable"); return; }
                    const std::vector<std::string> params{std::to_string(self->request.message->id), boxes->get_value(0, "mailbox_id"), boxes->get_value(0, "user_id")};
                    self->connection()->async_execute("INSERT INTO mail_mailbox (mail_id,mailbox_id,user_id) VALUES (?,?,?)", params,
                        [self](bool ok) { self->guard([self, ok] { if (!ok) self->fail("Sent relation insert failed"); else self->local_mailboxes_next(); }); });
                });
            });
    }
    void local_mailboxes_next() {
        const auto& message = *request.message;
        while (mailbox_index < message.to.size()) {
            const auto recipient = message.to[mailbox_index++];
            const auto recipient_domain = domain(recipient);
            if (recipient_domain.empty() || recipient_domain != lower(request.options.local_domain)) continue;
            // Preserve the existing non-critical local INBOX relation policy.
            // These execute synchronously only after the prior driver completion.
            connection()->execute(db::sql::build_insert_mailbox_for_recipient(message.id, recipient, connection()));
        }
        insert_attachments();
    }
    void insert_attachments() {
        if (request.message->attachments.empty()) { outbox_next(); return; }
        auto self = shared_from_this();
        connection()->async_execute(db::sql::build_insert_attachments(request.message->id, request.message->attachments, connection()),
            [self](bool ok) { self->guard([self, ok] { if (!ok) self->fail("Failed to insert attachment metadata"); else self->outbox_next(); }); });
    }
    void outbox_next() {
        const auto& message = *request.message;
        while (outbox_index < message.to.size()) {
            const auto recipient = message.to[outbox_index++];
            const auto recipient_domain = domain(recipient);
            if (recipient_domain.empty() || recipient_domain == lower(request.options.local_domain)) continue;
            const bool reserved = !request.options.reserve_owner.empty() && request.options.reserve_lease_seconds > 0;
            const auto sql = reserved
                ? db::sql::build_insert_outbox_reserved(message.id, message.from, recipient, request.options.reserve_owner, request.options.reserve_lease_seconds, connection())
                : db::sql::build_insert_outbox_pending(message.id, message.from, recipient, static_cast<int>(request.options.max_attempts), connection());
            auto self = shared_from_this();
            connection()->async_execute(sql, [self, recipient, reserved](bool ok) {
                self->guard([self, recipient, reserved, ok] {
                    if (!ok) { self->fail("Failed to insert outbox row"); return; }
                    self->connection()->async_query(db::sql::build_select_last_insert_id(), [self, recipient, reserved](std::shared_ptr<pr::IDBResult> rows) {
                        self->guard([self, rows, recipient, reserved] {
                            uint64_t id = 0;
                            if (!rows || !rows->get_row_count() || !parse_uint(rows->get_value(0, "id"), id) || !id) {
                                self->fail("Failed to fetch LAST_INSERT_ID"); return;
                            }
                            const auto& message = *self->request.message;
                            self->result.outbox_records.push_back({id, message.id, message.from, recipient, message.body_path,
                                reserved ? 1 : 0, reserved ? 8 : static_cast<int>(self->request.options.max_attempts)});
                            self->outbox_next();
                        });
                    });
                });
            });
            return;
        }
        auto self = shared_from_this();
        connection()->async_commit([self](bool ok) {
            self->guard([self, ok] {
                if (!ok) { self->fail("Failed to commit transaction"); return; }
                self->transaction = false; self->result.transaction = PersistenceTransaction::committed;
                self->finish(PersistenceDisposition::committed);
            });
        });
    }
};
class SqlPersistenceRepository final : public IPersistenceRepository {
    std::shared_ptr<router::IShardRouter> router_;
public:
    explicit SqlPersistenceRepository(std::shared_ptr<router::IShardRouter> router) : router_(std::move(router)) {}
    void persist_async(PersistenceRequest request, Complete complete) override {
        auto operation = std::make_shared<PrimaryOperation>();
        operation->router = router_; operation->request = std::move(request); operation->complete = std::move(complete);
        operation->start();
    }
    std::vector<RecipientPersistenceResult> persist_recipient_copies(const PersistenceRequest& request, int sender_shard) override {
        std::vector<RecipientPersistenceResult> results;
        if (!request.message || !router_) return results;
        const auto& message = *request.message;
        for (size_t i = 0; i < message.to.size(); ++i) {
            const auto& recipient = message.to[i];
            if (domain(recipient).empty() || domain(recipient) != lower(request.options.local_domain)) continue;
            RecipientPersistenceResult result; result.recipient = recipient;
            std::shared_ptr<DatabaseOwner> database;
            bool transaction = false;
            auto rollback = [&] {
                if (!transaction) return;
                transaction = false; bool ok = false;
                try { ok = database->connection()->rollback(); } catch (...) {}
                if (!ok) disconnect(database);
                result.transaction = ok ? PersistenceTransaction::rolled_back : PersistenceTransaction::rollback_failed;
            };
            try {
                result.shard = router_->route(recipient);
                if (result.shard < 0 || result.shard == sender_shard) continue;
                database = std::make_shared<DatabaseOwner>(router_->get_db_pool(static_cast<size_t>(result.shard)));
                if (!database->valid()) throw std::runtime_error("Recipient shard database unavailable");
                auto* conn = database->connection();
                transaction = true;
                if (!conn->begin_transaction()) { transaction = false; throw std::runtime_error("Failed to begin recipient transaction"); }
                if (!conn->execute(db::sql::build_insert_mail(message.id, message.subject, message.body_path, message.send_time, conn)))
                    throw std::runtime_error("Failed to insert recipient mail");
                const auto row_id = i < message.ids.size() ? message.ids[i] : 0;
                if (!conn->execute(db::sql::build_insert_recipient_single(message.id, row_id, message.from, recipient, 1, message.source_message_id, conn)))
                    throw std::runtime_error("Failed to insert recipient relation");
                if (!conn->execute(db::sql::build_insert_mailbox_for_recipient(message.id, recipient, conn)))
                    throw std::runtime_error("Failed to insert recipient mailbox");
                if (!conn->commit()) throw std::runtime_error("Failed to commit recipient transaction");
                transaction = false; result.ok = true; result.transaction = PersistenceTransaction::committed;
            } catch (const std::exception& error) { rollback(); result.error = error.what(); }
            catch (...) { rollback(); result.error = "Recipient persistence failed"; }
            results.push_back(std::move(result));
        }
        return results;
    }
    PersistenceEraseResult erase_metadata(const PersistenceEraseRequest& request) override {
        PersistenceEraseResult result;
        if (request.mail_ids.empty()) return result;
        try {
            auto pool = router_ ? router_->get_db_pool(static_cast<size_t>(request.shard)) : nullptr;
            if (!pool) return result;
            auto database = std::make_shared<DatabaseOwner>(std::move(pool));
            if (!database->valid()) return {false, "Cleanup database unavailable"};
            auto* conn = database->connection();
            auto erase = [&](const std::string& sql) {
                try { if (!conn->execute(sql)) result.ok = false; }
                catch (...) { result.ok = false; }
            };
            if (request.delete_attachment_metadata)
                for (auto id : request.mail_ids) erase(db::sql::build_delete_attachments_by_mail(id));
            std::string ids;
            for (auto id : request.mail_ids) { if (!ids.empty()) ids += ','; ids += std::to_string(id); }
            erase(db::sql::build_delete_mail_recipients_by_id_list(ids));
            erase(db::sql::build_delete_mails_by_id_list(ids));
            if (!result.ok) result.error = "Metadata cleanup incomplete";
        } catch (const std::exception& error) { result = {false, error.what()}; }
        catch (...) { result = {false, "Metadata cleanup failed"}; }
        return result;
    }
};
} // namespace
std::shared_ptr<IPersistenceRepository> make_sql_persistence_repository(std::shared_ptr<router::IShardRouter> router) {
    return std::make_shared<SqlPersistenceRepository>(std::move(router));
}
} // namespace mail_system::repository
