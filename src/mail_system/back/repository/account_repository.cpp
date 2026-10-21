#include "mail_system/back/repository/account_repository.h"
#include "mail_system/back/common/safe_parse.h"
#include "mail_system/back/db/sql_queries.h"
#include "mail_system/back/router/i_shard_router.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mail_system::repository {
namespace {
// Member destruction releases the ScopedConnection before destroying its pool.
struct AccountLease {
    std::shared_ptr<pr::DBPool> pool;
    std::shared_ptr<pr::ScopedConnection> connection;
};
std::shared_ptr<AccountLease> acquire(const std::shared_ptr<router::IShardRouter>& router, int shard) {
    auto pool = router && shard >= 0 ? router->get_db_pool(static_cast<size_t>(shard)) : nullptr;
    if (!pool) return {};
    auto connection = pool->acquire_connection();
    if (!connection || !connection->is_valid()) return {};
    return std::make_shared<AccountLease>(AccountLease{std::move(pool), std::move(connection)});
}
CredentialLookup credentials(const std::shared_ptr<pr::IDBResult>& rows) {
    if (!rows) return {};
    if (!rows->get_row_count()) return {AccountLookupStatus::not_found, {}};
    return {AccountLookupStatus::found,
            {safe_stoull(rows->get_value(0, "id")), safe_stoull(rows->get_value(0, "status")),
             rows->get_value(0, "password")}};
}
class SqlCredentialAccess final : public IAccountCredentialAccess {
public:
    SqlCredentialAccess(std::shared_ptr<AccountLease> lease, std::string address, int shard)
        : lease_(std::move(lease)), address_(std::move(address)), shard_(shard) {}
    int shard() const override { return shard_; }
    void read_async(ReadComplete complete) override {
        auto lease = lease_;
        (*lease->connection)->async_query(db::sql::build_auth_user_query(), {address_},
            [lease, complete = std::move(complete)](std::shared_ptr<pr::IDBResult> rows) {
                if (complete) complete(credentials(rows));
            });
    }
    void upgrade_legacy_hash_async(const std::string& expected, const std::string& replacement,
                                   Complete complete) override {
        auto lease = lease_;
        (*lease->connection)->async_execute(db::sql::build_update_password_hash(),
            {replacement, address_, expected}, [lease, complete = std::move(complete)](bool ok) {
                if (complete) complete(ok);
            });
    }
    void touch_login_async(Complete complete) override {
        auto lease = lease_;
        (*lease->connection)->async_execute(db::sql::build_update_last_login(), {address_},
            [lease, complete = std::move(complete)](bool ok) { if (complete) complete(ok); });
    }
private:
    std::shared_ptr<AccountLease> lease_;
    std::string address_;
    int shard_;
};
class SqlAccountRepository final : public IAccountRepository {
public:
    explicit SqlAccountRepository(std::shared_ptr<router::IShardRouter> router) : router_(std::move(router)) {}
    int route_address(const std::string& address) override { return router_ ? router_->route(address) : 0; }
    std::shared_ptr<IAccountCredentialAccess> open_credentials(const std::string& address, int shard) override {
        auto lease = acquire(router_, shard);
        if (!lease) return {};
        return std::make_shared<SqlCredentialAccess>(std::move(lease), address, shard);
    }
    void recipient_async(const std::string& address, std::function<void(RecipientLookup)> complete) override {
        auto lease = acquire(router_, policy_shard(address));
        if (!lease) { if (complete) complete({AccountLookupStatus::unavailable, 0}); return; }
        (*lease->connection)->async_query(db::sql::build_recipient_exists_query(), {address},
            [lease, complete = std::move(complete)](std::shared_ptr<pr::IDBResult> rows) {
                RecipientLookup out;
                if (rows) {
                    out.status = rows->get_row_count() ? AccountLookupStatus::found : AccountLookupStatus::not_found;
                    if (rows->get_row_count()) out.account_status = safe_stoull(rows->get_value(0, "status"));
                }
                if (complete) complete(out);
            });
    }
    void aliases_async(const std::string& address, std::function<void(AliasLookup)> complete) override {
        auto lease = acquire(router_, policy_shard(address));
        if (!lease) { if (complete) complete({}); return; }
        (*lease->connection)->async_query(db::sql::build_alias_targets_query(), {address},
            [lease, complete = std::move(complete)](std::shared_ptr<pr::IDBResult> rows) {
                AliasLookup out;
                out.success = static_cast<bool>(rows);
                if (rows) for (size_t i = 0; i < rows->get_row_count(); ++i)
                    out.targets.push_back(rows->get_value(i, "target_address"));
                if (complete) complete(std::move(out));
            });
    }
    void consume_quota_async(const std::string& address, int limit,
                              std::function<void(QuotaOutcome)> complete) override {
        if (!complete) return;
        if (limit <= 0) { complete(QuotaOutcome::consumed); return; }
        auto lease = acquire(router_, policy_shard(address));
        if (!lease) { complete(QuotaOutcome::unavailable); return; }
        (*lease->connection)->async_execute(db::sql::build_increment_sent_today_query(),
            {address, std::to_string(limit)}, [lease, complete = std::move(complete)](bool ok) mutable {
                if (!ok) { complete(QuotaOutcome::update_failed); return; }
                (*lease->connection)->async_query(db::sql::build_select_row_count(),
                    [lease, complete = std::move(complete)](std::shared_ptr<pr::IDBResult> rows) {
                        if (!rows || !rows->get_row_count()) { complete(QuotaOutcome::count_failed); return; }
                        size_t affected;
                        try { affected = static_cast<size_t>(std::stoull(rows->get_value(0, "affected"))); }
                        catch (...) { complete(QuotaOutcome::count_failed); return; }
                        complete(affected ? QuotaOutcome::consumed : QuotaOutcome::exceeded);
                    });
            });
    }
    ActorStatus verify_actor(const std::string& address, uint64_t user_id, int shard) override {
        auto lease = acquire(router_, shard);
        if (!lease) return ActorStatus::unavailable;
        auto rows = (*lease->connection)->query(db::sql::build_auth_user_query(), {address});
        if (!rows) return ActorStatus::unavailable;
        if (!rows->get_row_count() || safe_stoull(rows->get_value(0, "id")) != user_id ||
            safe_stoull(rows->get_value(0, "status")) != 1) return ActorStatus::denied;
        return ActorStatus::valid;
    }
private:
    int policy_shard(const std::string& address) { const int shard = route_address(address); return shard < 0 ? 0 : shard; }
    std::shared_ptr<router::IShardRouter> router_;
};
} // namespace
std::shared_ptr<IAccountRepository> make_account_repository(std::shared_ptr<router::IShardRouter> router) {
    return std::make_shared<SqlAccountRepository>(std::move(router));
}
} // namespace mail_system::repository
