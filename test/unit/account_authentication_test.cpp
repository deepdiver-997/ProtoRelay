#undef NDEBUG
#include "mail_system/back/common/account_authentication.h"
#include "mail_system/back/common/account_policy.h"
#include "mail_system/back/common/bcrypt.h"
#include "mail_system/back/common/credential_stamp.h"
#include "mail_system/back/router/static_shard_router.h"
#include "mock_db_pool.h"
#include "fake_account_repository.h"
#include <cassert>
#include <deque>
#include <iostream>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace mail_system;
using test::MockDbPool;
using test::MockDbResult;
namespace {
const std::string address = "alice@test.local";
std::shared_ptr<pr::IDBResult> user(const std::string& password, const std::string& status = "1") {
    return std::make_shared<MockDbResult>(std::vector<std::map<std::string, std::string>>{
        {{"id", "42"}, {"status", status}, {"password", password}}});
}
struct Fixture {
    std::shared_ptr<MockDbPool> pool = std::make_shared<MockDbPool>();
    std::shared_ptr<AuthCache> cache = std::make_shared<AuthCache>();
    std::shared_ptr<router::StaticShardRouter> router = std::make_shared<router::StaticShardRouter>(
        std::vector<std::pair<std::string, int>>{}, 0,
        std::vector<std::shared_ptr<pr::DBPool>>{pool},
        std::vector<std::shared_ptr<pr::storage::IStorageProvider>>{});
    AccountAuthResult result;
    int calls = 0;
    IAccountAuthenticator::Complete callback() {
        return [this](AccountAuthResult value) { ++calls; result = std::move(value); };
    }
};
struct ScheduledWorkers final : pr::ThreadPoolBase {
    std::deque<std::function<void()>> tasks;
    void start() override {}
    void stop(bool = true) override {}
    size_t thread_count() const override { return 1; }
    bool is_running() const override { return true; }
    void drain() { while (!tasks.empty()) { auto task = std::move(tasks.front()); tasks.pop_front(); task(); } }
    void post_impl(std::function<void()> task) override { tasks.push_back(std::move(task)); }
};
struct StubAuthenticator final : IAccountAuthenticator {
    int calls = 0;
    bool requested_fresh = false;
    void login_async(const std::string& who, const std::string&, Complete complete, bool fresh) override {
        ++calls;
        requested_fresh = fresh;
        AccountAuthResult result;
        result.ok = true; result.user_id = 77; result.address = who;
        complete(std::move(result));
    }
};
void unavailable_and_cache(const std::string& hash) {
    Fixture f;
    AccountAuthenticationService unavailable(nullptr, f.cache, nullptr);
    unavailable.login_async(address, "pw", f.callback());
    assert(f.calls == 1 && !f.result.ok && f.result.server_error);
    f.calls = 0;
    f.cache->store(address, AuthCacheEntry{hash, 1, 42, 3});
    unavailable.login_async(address, "pw", f.callback());
    assert(f.calls == 1 && f.result.ok && f.result.user_id == 42 && f.result.shard == 3);
    assert(f.result.credential_stamp == credential_stamp(hash));
    assert(!f.pool->mock_conn()->has_pending_query());
    f.calls = 0;
    unavailable.login_async(address, "wrong", f.callback());
    assert(f.calls == 1 && !f.result.ok && !f.result.server_error);
    f.cache->store(address, AuthCacheEntry{hash, 0, 42, 0});
    f.calls = 0;
    unavailable.login_async(address, "pw", f.callback());
    assert(f.calls == 1 && !f.result.ok && !f.result.server_error);
}
void query_errors(const std::string& hash) {
    for (int scenario = 0; scenario < 4; ++scenario) {
        Fixture f;
        AccountAuthenticationService service(f.router, f.cache, nullptr);
        service.login_async(address, "pw", f.callback());
        assert(f.calls == 0 && f.pool->mock_conn()->has_pending_query());
        switch (scenario) {
        case 0: f.pool->mock_conn()->fire_query(nullptr); break;
        case 1: f.pool->mock_conn()->fire_query(std::make_shared<MockDbResult>()); break;
        case 2: f.pool->mock_conn()->fire_query(user(hash, "0")); break;
        default: f.pool->mock_conn()->fire_query(user(bcrypt_hash("another", 4))); break;
        }
        assert(f.calls == 1 && !f.result.ok && f.result.server_error == (scenario == 0));
        assert(!f.pool->mock_conn()->has_pending_execute());
    }
}
void ordered_completion(const std::string& hash) {
    Fixture f;
    {
        AccountAuthenticationService service(f.router, nullptr, nullptr); // Cache is optional.
        service.login_async(address, "pw", f.callback());
    } // An in-flight operation does not depend on the facade object's lifetime.
    f.pool->mock_conn()->fire_query(user(hash));
    assert(f.calls == 0 && f.pool->mock_conn()->has_pending_execute());
    assert(f.pool->mock_conn()->last_query_sql().find("last_login") != std::string::npos);
    f.pool->mock_conn()->fire_execute(false); // Last-login failure does not reject verified credentials.
    assert(f.calls == 1 && f.result.ok && f.result.user_id == 42);
    f.pool->mock_conn()->fire_execute(true);
    assert(f.calls == 1);
}
void legacy_rehash_order() {
    Fixture f;
    AccountAuthenticationService service(f.router, f.cache, nullptr);
    service.login_async(address, "pw", f.callback());
    f.pool->mock_conn()->fire_query(user("pw"));
    assert(f.calls == 0 && f.pool->mock_conn()->has_pending_execute());
    auto params = f.pool->mock_conn()->last_query_params();
    assert(params.size() == 3 && is_bcrypt_hash(params[0]) && params[1] == address && params[2] == "pw");
    const auto upgraded = params[0];
    f.pool->mock_conn()->fire_execute(true);
    assert(f.calls == 0 && f.pool->mock_conn()->has_pending_execute());
    assert(f.pool->mock_conn()->last_query_sql().find("last_login") != std::string::npos);
    AuthCacheEntry entry;
    assert(f.cache->lookup(address, entry) && entry.password_hash == upgraded && entry.user_id == 42);
    f.pool->mock_conn()->fire_execute(true);
    assert(f.calls == 1 && f.result.ok);
}
void fresh_rechecks_credentials(const std::string& hash) {
    Fixture f;
    f.cache->store(address, AuthCacheEntry{hash, 1, 42, 0});
    AccountAuthenticationService service(f.router, f.cache, nullptr);
    service.login_async(address, "pw", f.callback(), true);
    assert(f.pool->mock_conn()->has_pending_query()); // Fresh web login bypasses cached credentials.
    f.pool->mock_conn()->fire_query(user(hash));
    assert(f.calls == 0 && f.pool->mock_conn()->has_pending_query());
    f.pool->mock_conn()->fire_query(user(bcrypt_hash("reset-by-admin", 4)));
    assert(f.calls == 1 && !f.result.ok && !f.result.server_error);
    assert(!f.pool->mock_conn()->has_pending_execute());
}
void database_callback_returns_to_worker(const std::string& hash) {
    Fixture f;
    auto workers = std::make_shared<ScheduledWorkers>();
    AccountAuthenticationService service(f.router, f.cache, workers);
    service.login_async(address, "pw", f.callback());
    assert(!f.pool->mock_conn()->has_pending_query());
    workers->drain();
    assert(f.pool->mock_conn()->has_pending_query());
    f.pool->mock_conn()->fire_query(user(hash));
    // A DB callback must schedule verification rather than hash on its calling IO thread.
    assert(!workers->tasks.empty() && !f.pool->mock_conn()->has_pending_execute());
    workers->drain();
    assert(f.pool->mock_conn()->has_pending_execute() && f.calls == 0);
    f.pool->mock_conn()->fire_execute(true);
    assert(f.calls == 1 && f.result.ok);
}
void policy_accepts_authentication_port() {
    auto fake = std::make_shared<StubAuthenticator>();
    AccountPolicy policy(nullptr, nullptr, nullptr, fake);
    AccountAuthResult result;
    policy.login_async(address, "ignored", [&](AccountAuthResult value) { result = std::move(value); }, true);
    assert(fake->calls == 1 && fake->requested_fresh && result.ok && result.user_id == 77);
}

void default_login_keeps_pool_until_final_callback(const std::string& hash) {
    struct State { int returned = 0, destroyed = 0; };
    struct Pool final : test::MockDbPool {
        std::shared_ptr<State> state;
        explicit Pool(std::shared_ptr<State> value) : state(std::move(value)) {}
        ~Pool() override { assert(state->returned == 1); ++state->destroyed; }
        void release_connection(std::shared_ptr<pr::IDBConnection>) override { ++state->returned; }
    };
    auto state = std::make_shared<State>();
    auto pool = std::make_shared<Pool>(state); auto db = pool->mock_conn();
    auto router = std::make_shared<router::StaticShardRouter>(
        std::vector<std::pair<std::string, int>>{}, 0, std::vector<std::shared_ptr<pr::DBPool>>{pool},
        std::vector<std::shared_ptr<pr::storage::IStorageProvider>>{});
    std::weak_ptr<Pool> weak = pool;
    bool done = false;
    {
        AccountAuthenticationService service(router, nullptr, nullptr);
        service.login_async(address, "pw", [&](AccountAuthResult result) {
            assert(result.ok && !weak.expired() && !state->returned); done = true;
        });
    }
    router.reset(); pool.reset(); assert(!weak.expired());
    db->fire_query(user(hash));
    assert(!done && db->has_pending_execute() && !weak.expired());
    db->fire_execute(true);
    assert(done && weak.expired() && state->returned == 1 && state->destroyed == 1);
}
void injected_login_preserves_worker_and_fresh_contract(const std::string& hash) {
    using namespace repository;
    auto accounts = std::make_shared<test::FakeAccountRepository>();
    auto access = accounts->access;
    auto cache = std::make_shared<AuthCache>();
    cache->store(address, AuthCacheEntry{hash, 1, 999, 9});
    auto workers = std::make_shared<ScheduledWorkers>();
    int calls = 0; AccountAuthResult out;
    {
        AccountAuthenticationService service(nullptr, cache, workers, accounts);
        service.login_async(address, "pw", [&](AccountAuthResult result) { ++calls; out = std::move(result); }, true);
    }
    assert(!accounts->opens); workers->drain();
    assert(accounts->opens == 1 && accounts->opened_shard == 3 && accounts->opened_address == address);
    accounts.reset(); // Pending work needs only the independent credential context.
    access->finish_read({AccountLookupStatus::found, {42, 1, hash}});
    assert(!access->touches && !workers->tasks.empty()); workers->drain();
    assert(access->reads == 2 && !access->touches && !access->upgrades && !calls);
    access->finish_read({AccountLookupStatus::found, {42, 1, hash}});
    assert(!access->touches); workers->drain();
    assert(access->touches == 1 && !calls);
    access->finish_touch(false);
    assert(calls == 1 && out.ok && out.user_id == 42 && out.shard == 3 && out.credential_stamp == credential_stamp(hash));
}
void injected_cas_miss_cannot_issue_a_fresh_session() {
    using namespace repository;
    auto accounts = std::make_shared<test::FakeAccountRepository>();
    auto access = accounts->access;
    auto cache = std::make_shared<AuthCache>();
    AccountAuthenticationService service(nullptr, cache, nullptr, accounts);
    int calls = 0;
    service.login_async(address, "legacy", [&](AccountAuthResult result) {
        assert(!result.ok && !result.server_error); ++calls;
    }, true);
    access->finish_read({AccountLookupStatus::found, {42, 1, "legacy"}});
    assert(access->upgrades == 1 && access->expected_hash == "legacy" && !calls);
    access->finish_upgrade(true); // A successful CAS execution may affect zero rows.
    assert(access->reads == 2);
    AuthCacheEntry cached;
    assert(cache->lookup(address, cached) && cached.password_hash == access->replacement_hash);
    access->finish_read({AccountLookupStatus::found, {42, 1, bcrypt_hash("reset", 4)}});
    assert(calls == 1 && !access->touches);
}
void fresh_login_failure_mapping(const std::string& hash) {
    using namespace repository;
    for (const auto& live : std::vector<CredentialLookup>{{AccountLookupStatus::query_failed, {}},
             {AccountLookupStatus::not_found, {}}, {AccountLookupStatus::found, {41, 1, hash}},
             {AccountLookupStatus::found, {42, 0, hash}}}) {
        auto accounts = std::make_shared<test::FakeAccountRepository>();
        auto access = accounts->access;
        AccountAuthenticationService service(nullptr, nullptr, nullptr, accounts);
        int calls = 0;
        service.login_async(address, "pw", [&](AccountAuthResult result) {
            assert(!result.ok && result.server_error == (live.status == AccountLookupStatus::query_failed)); ++calls;
        }, true);
        access->finish_read({AccountLookupStatus::found, {42, 1, hash}});
        access->finish_read(live);
        assert(calls == 1 && !access->touches && !access->upgrades);
    }
}
void policy_cache_and_failure_contracts() {
    using namespace repository;
    auto accounts = std::make_shared<test::FakeAccountRepository>();
    auto recipients = std::make_shared<AuthCache>();
    auto aliases = std::make_shared<AccountPolicy::AliasCache>(10, std::chrono::seconds(300));
    AccountPolicy policy(nullptr, nullptr, nullptr, nullptr, accounts);
    policy.set_recipient_caches(recipients, aliases);
    int calls = 0;
    accounts->recipient_result = {AccountLookupStatus::unavailable, 0};
    policy.user_exists_async(address, [&](bool ok) { assert(!ok); ++calls; });
    AuthCacheEntry entry; assert(!recipients->lookup(address, entry));
    accounts->recipient_result = {AccountLookupStatus::query_failed, 0};
    policy.user_exists_async(address, [&](bool ok) { assert(!ok); ++calls; });
    assert(recipients->lookup(address, entry) && !entry.status);
    policy.user_exists_async(address, [&](bool ok) { assert(!ok); ++calls; });
    assert(accounts->recipient_calls == 2); // Query failure has the historical negative TTL.
    accounts->alias_result = {false, {}};
    for (int i = 0; i != 2; ++i) policy.alias_targets_async(address, [&](auto targets) { assert(targets.empty()); ++calls; });
    assert(accounts->alias_calls == 2); // Alias errors are never negatively cached.
    accounts->alias_result = {true, {}};
    for (int i = 0; i != 2; ++i) policy.alias_targets_async(address, [&](auto targets) { assert(targets.empty()); ++calls; });
    assert(accounts->alias_calls == 3); // Confirmed non-alias is cached.
    for (auto outcome : {QuotaOutcome::consumed, QuotaOutcome::exceeded, QuotaOutcome::unavailable,
                         QuotaOutcome::update_failed, QuotaOutcome::count_failed}) {
        accounts->quota_result = outcome;
        policy.check_send_quota_async(address, 5, [&, outcome](bool allowed) {
            assert(allowed == (outcome == QuotaOutcome::consumed || outcome == QuotaOutcome::unavailable ||
                               outcome == QuotaOutcome::update_failed)); ++calls;
        });
    }
    policy.check_send_quota_async(address, 0, [&](bool ok) { assert(ok); ++calls; });
    assert(accounts->quota_calls == 5 && accounts->quota_limit == 5 && accounts->quota_address == address && calls == 13);
}
void delayed_policy_survives_facade_destruction() {
    using namespace repository;
    auto accounts = std::make_shared<test::FakeAccountRepository>();
    accounts->defer_alias = true;
    std::weak_ptr<test::FakeAccountRepository> weak = accounts;
    bool done = false;
    {
        AccountPolicy policy(nullptr, nullptr, nullptr, nullptr, accounts);
        policy.alias_targets_async(address, [&](auto targets) { assert(targets == std::vector<std::string>{"b@test"}); done = true; });
    }
    auto complete = std::move(accounts->alias_complete);
    accounts.reset(); assert(!weak.expired());
    complete({true, {"b@test"}}); complete = {};
    assert(done && weak.expired());
}
} // namespace
int main() {
    const auto hash = bcrypt_hash("pw", 4);
    unavailable_and_cache(hash);
    query_errors(hash);
    ordered_completion(hash);
    legacy_rehash_order();
    fresh_rechecks_credentials(hash);
    database_callback_returns_to_worker(hash);
    policy_accepts_authentication_port();
    default_login_keeps_pool_until_final_callback(hash);
    injected_login_preserves_worker_and_fresh_contract(hash);
    injected_cas_miss_cannot_issue_a_fresh_session();
    fresh_login_failure_mapping(hash);
    policy_cache_and_failure_contracts();
    delayed_policy_survives_facade_destruction();
    std::cout << "account_authentication_test: passed\n";
}
