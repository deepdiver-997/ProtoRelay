#undef NDEBUG
#include "mail_system/back/repository/account_repository.h"
#if defined(BOOST_ASIO_VERSION) || defined(BOOST_VERSION)
#error "Account repository contracts must not expose an Asio backend"
#endif
#include "mail_system/back/router/static_shard_router.h"
#include "mock_db_pool.h"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace mail_system;
using namespace mail_system::repository;
using test::MockDbResult;
struct Counts { int gets = 0, returns = 0, destroyed = 0; };
struct RecordingConnection final : test::MockDbConnection {
    std::string sync_sql;
    std::vector<std::string> sync_params;
    std::shared_ptr<pr::IDBResult> query(const std::string& sql,
                                       const std::vector<std::string>& params) override {
        sync_sql = sql; sync_params = params;
        return pop_sync_result();
    }
};
class OneSlotPool final : public pr::DBPool {
public:
    std::shared_ptr<RecordingConnection> db = std::make_shared<RecordingConnection>();
    std::shared_ptr<Counts> counts = std::make_shared<Counts>();
    bool unavailable = false;
    ~OneSlotPool() override { assert(!busy_); ++counts->destroyed; }
    size_t get_pool_size() const override { return 1; }
    size_t get_available_connections() const override { return busy_ ? 0 : 1; }
    size_t get_max_pool_size() const override { return 1; }
    size_t get_active_connections() const override { return busy_ ? 1 : 0; }
    void close() override {}
    std::shared_ptr<pr::IDBConnection> get_connection() override {
        ++counts->gets;
        if (unavailable) return {};
        assert(!busy_); busy_ = true; return db;
    }
    void release_connection(std::shared_ptr<pr::IDBConnection> connection) override {
        assert(busy_ && connection == db); busy_ = false; ++counts->returns;
    }
protected:
    void initialize_pool() override {}
    std::shared_ptr<pr::IDBConnection> create_connection() override { return db; }
private:
    bool busy_ = false;
};
auto rows(std::vector<std::map<std::string, std::string>> values = {}) {
    return std::make_shared<MockDbResult>(std::move(values));
}
auto user(std::string id = "42", std::string status = "1", std::string hash = "legacy") {
    return rows({{{"id", std::move(id)}, {"status", std::move(status)}, {"password", std::move(hash)}}});
}
struct Fixture {
    std::shared_ptr<OneSlotPool> pool = std::make_shared<OneSlotPool>();
    std::shared_ptr<router::StaticShardRouter> router = std::make_shared<router::StaticShardRouter>(
        std::vector<std::pair<std::string, int>>{}, 0,
        std::vector<std::shared_ptr<pr::DBPool>>{pool},
        std::vector<std::shared_ptr<pr::storage::IStorageProvider>>{});
    std::shared_ptr<IAccountRepository> repository = make_account_repository(router);
};
const std::string address = "alice@test.local";
void credential_sequence_and_mapping() {
    Fixture f;
    assert(f.repository->route_address(address) == 0);
    auto access = f.repository->open_credentials(address, 0);
    assert(access && access->shard() == 0 && f.pool->counts->gets == 1);
    int calls = 0;
    access->read_async([&](CredentialLookup result) {
        assert(result.status == AccountLookupStatus::found && result.credentials.user_id == 42 &&
               result.credentials.status == 1 && result.credentials.stored_hash == "legacy"); ++calls;
    });
    assert(f.pool->db->last_query_params() == std::vector<std::string>{address});
    f.pool->db->fire_query(user());
    access->upgrade_legacy_hash_async("legacy", "replacement", [&](bool ok) { assert(ok); ++calls; });
    assert(f.pool->db->last_query_sql().find("AND password = ?") != std::string::npos);
    assert((f.pool->db->last_query_params() == std::vector<std::string>{"replacement", address, "legacy"}));
    // Execution success includes zero affected rows. No ROW_COUNT query may be
    // inserted here: the service retains its documented cache/CAS semantics.
    f.pool->db->fire_execute(true);
    assert(!f.pool->db->has_pending_query());
    access->read_async([&](CredentialLookup result) {
        assert(result.credentials.stored_hash == "admin-reset"); ++calls;
    });
    f.pool->db->fire_query(user("42", "1", "admin-reset"));
    access->touch_login_async([&](bool ok) { assert(!ok); ++calls; });
    assert(f.pool->db->last_query_sql().find("last_login_time") != std::string::npos);
    f.pool->db->fire_execute(false);
    assert(calls == 4 && f.pool->counts->gets == 1 && !f.pool->counts->returns);
    for (const auto& result : std::vector<std::shared_ptr<pr::IDBResult>>{nullptr, rows(), user("bad", "bad")}) {
        access->read_async([&](CredentialLookup value) {
            if (!result) assert(value.status == AccountLookupStatus::query_failed);
            else if (!result->get_row_count()) assert(value.status == AccountLookupStatus::not_found);
            else assert(value.status == AccountLookupStatus::found && !value.credentials.user_id && !value.credentials.status);
            ++calls;
        });
        f.pool->db->fire_query(result);
    }
    access.reset(); assert(f.pool->counts->returns == 1 && calls == 7);
}
void disconnected_credential_context_does_not_start_more_work() {
    Fixture f;
    auto access = f.repository->open_credentials(address, 0);
    f.pool->db->disconnect();
    int calls = 0;
    access->read_async([&](CredentialLookup result) { assert(result.status == AccountLookupStatus::query_failed); ++calls; });
    access->upgrade_legacy_hash_async("legacy", "new", [&](bool ok) { assert(!ok); ++calls; });
    access->touch_login_async([&](bool ok) { assert(!ok); ++calls; });
    assert(calls == 3 && !f.pool->db->has_pending_query() && !f.pool->db->has_pending_execute());
    access.reset(); assert(f.pool->counts->returns == 1);
}
void delayed_credentials_keep_the_pool_alive() {
    Fixture f;
    auto db = f.pool->db; auto counts = f.pool->counts;
    std::weak_ptr<OneSlotPool> weak = f.pool;
    auto access = f.repository->open_credentials(address, 0);
    bool done = false;
    access->read_async([&](CredentialLookup result) {
        assert(result.status == AccountLookupStatus::found && !counts->returns && !weak.expired()); done = true;
    });
    access.reset(); f.repository.reset(); f.router.reset(); f.pool.reset();
    assert(!weak.expired()); db->fire_query(user());
    assert(done && weak.expired() && counts->returns == 1 && counts->destroyed == 1);
}
void recipients_and_aliases() {
    Fixture f; int calls = 0;
    for (const auto& result : std::vector<std::shared_ptr<pr::IDBResult>>{nullptr, rows(), user("42", "0"), user()}) {
        f.repository->recipient_async(address, [&](RecipientLookup value) {
            if (!result) assert(value.status == AccountLookupStatus::query_failed);
            else if (!result->get_row_count()) assert(value.status == AccountLookupStatus::not_found);
            else assert(value.status == AccountLookupStatus::found && value.account_status == (result->get_value(0, "status") == "1"));
            ++calls;
        });
        assert(f.pool->db->last_query_params() == std::vector<std::string>{address});
        f.pool->db->fire_query(result);
    }
    for (const auto& result : std::vector<std::shared_ptr<pr::IDBResult>>{nullptr, rows(), rows({{{"target_address", "b@test"}}, {{"target_address", "a@test"}}})}) {
        f.repository->aliases_async(address, [&](AliasLookup value) {
            assert(value.success == static_cast<bool>(result));
            if (result && result->get_row_count()) assert((value.targets == std::vector<std::string>{"b@test", "a@test"}));
            else assert(value.targets.empty());
            ++calls;
        });
        assert(f.pool->db->last_query_sql().find("u.status = 1") != std::string::npos);
        f.pool->db->fire_query(result);
    }
    assert(calls == 7 && f.pool->counts->gets == 7 && f.pool->counts->returns == 7);
    f.pool->unavailable = true;
    assert(!f.repository->open_credentials(address, 0));
    f.repository->recipient_async(address, [](RecipientLookup r) { assert(r.status == AccountLookupStatus::unavailable); });
    f.repository->aliases_async(address, [](AliasLookup r) { assert(!r.success && r.targets.empty()); });
}
void quota_sequence_and_failures() {
    Fixture f; int calls = 0;
    f.repository->consume_quota_async(address, 0, [&](QuotaOutcome result) { assert(result == QuotaOutcome::consumed); ++calls; });
    assert(!f.pool->counts->gets);
    f.repository->consume_quota_async(address, 3, [&](QuotaOutcome result) { assert(result == QuotaOutcome::update_failed); ++calls; });
    assert((f.pool->db->last_query_params() == std::vector<std::string>{address, "3"}));
    f.pool->db->fire_execute(false); assert(!f.pool->db->has_pending_query());
    for (const std::string& affected : {std::string("1"), std::string("0"), std::string("bad"), std::string("empty"), std::string("null")}) {
        const int gets = f.pool->counts->gets;
        f.repository->consume_quota_async(address, 3, [&](QuotaOutcome result) {
            const auto expected = affected == "1" ? QuotaOutcome::consumed :
                affected == "0" ? QuotaOutcome::exceeded : QuotaOutcome::count_failed;
            assert(result == expected && f.pool->counts->gets == gets + 1 && f.pool->get_active_connections() == 1); ++calls;
        });
        f.pool->db->fire_execute(true);
        assert(f.pool->db->last_query_sql().find("ROW_COUNT()") != std::string::npos && f.pool->get_active_connections() == 1);
        f.pool->db->fire_query(affected == "null" ? nullptr : affected == "empty" ? rows() : rows({{{"affected", affected}}}));
    }
    assert(calls == 7 && f.pool->counts->gets == 6 && f.pool->counts->returns == 6);
    auto db = f.pool->db; auto counts = f.pool->counts; std::weak_ptr<OneSlotPool> weak = f.pool;
    f.repository->consume_quota_async(address, 3, [&](QuotaOutcome result) {
        assert(result == QuotaOutcome::consumed && !weak.expired()); ++calls;
    });
    f.repository.reset(); f.router.reset(); f.pool.reset();
    db->fire_execute(true); assert(!weak.expired()); db->fire_query(rows({{{"affected", "1"}}}));
    assert(weak.expired() && counts->returns == 7 && calls == 8);
}
void fresh_actor_releases_lease_and_denies_stale_accounts() {
    Fixture f;
    for (const auto& result : std::vector<std::shared_ptr<pr::IDBResult>>{nullptr, rows(), user("41"), user("42", "0"), user()}) {
        f.pool->db->push_sync_result(result);
        const auto status = f.repository->verify_actor(address, 42, 0);
        assert(status == (!result ? ActorStatus::unavailable : result->get_row_count() &&
            result->get_value(0, "id") == "42" && result->get_value(0, "status") == "1" ? ActorStatus::valid : ActorStatus::denied));
        assert(f.pool->db->sync_params == std::vector<std::string>{address});
        assert(!f.pool->get_active_connections());
        // The immediately following policy operation can use a one-slot pool.
        f.repository->consume_quota_async(address, 1, [](QuotaOutcome r) { assert(r == QuotaOutcome::update_failed); });
        f.pool->db->fire_execute(false);
    }
    auto unavailable = make_account_repository(nullptr);
    assert(!unavailable->open_credentials(address, 0));
    assert(unavailable->verify_actor(address, 42, 0) == ActorStatus::unavailable);
    unavailable->consume_quota_async(address, 1, [](QuotaOutcome r) { assert(r == QuotaOutcome::unavailable); });
    // The repository exposes unresolved routes, but policy operations retain
    // the existing fallback to pool zero.
    auto router = std::make_shared<router::StaticShardRouter>(std::vector<std::pair<std::string, int>>{}, -1,
        std::vector<std::shared_ptr<pr::DBPool>>{f.pool}, std::vector<std::shared_ptr<pr::storage::IStorageProvider>>{});
    auto repository = make_account_repository(router);
    assert(repository->route_address(address) == -1);
    repository->recipient_async(address, [](RecipientLookup r) { assert(r.status == AccountLookupStatus::not_found); });
    f.pool->db->fire_query(rows());
}
} // namespace
int main() {
    credential_sequence_and_mapping(); disconnected_credential_context_does_not_start_more_work();
    delayed_credentials_keep_the_pool_alive();
    recipients_and_aliases(); quota_sequence_and_failures(); fresh_actor_releases_lease_and_denies_stale_accounts();
    std::cout << "Account repository contracts passed\n";
}
