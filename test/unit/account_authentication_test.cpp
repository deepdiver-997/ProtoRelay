#undef NDEBUG
#include "mail_system/back/common/account_authentication.h"
#include "mail_system/back/common/account_policy.h"
#include "mail_system/back/common/bcrypt.h"
#include "mail_system/back/common/credential_stamp.h"
#include "mail_system/back/router/static_shard_router.h"
#include "mock_db_pool.h"
#include <cassert>
#include <deque>
#include <iostream>

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
    std::cout << "account_authentication_test: passed\n";
}
