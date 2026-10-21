#undef NDEBUG
#include "mail_system/back/repository/webmail_action_repository.h"
#include "mail_system/back/repository/web_session_repository.h"
#if defined(BOOST_VERSION) || defined(BOOST_ASIO_VERSION)
#error "Webmail persistence interfaces must not expose a network backend"
#endif
#include "mail_system/back/common/credential_stamp.h"
#include "mail_system/back/router/static_shard_router.h"
#include "mock_db_pool.h"
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace mail_system;
using Rows = std::vector<std::map<std::string, std::string>>;
std::shared_ptr<test::MockDbResult> rows(Rows value = {}) {
    return std::make_shared<test::MockDbResult>(std::move(value));
}
struct Statement { std::string sql; std::vector<std::string> parameters; };
struct Connection final : test::MockDbConnection {
    std::vector<Statement> queries, writes;
    int begins = 0, commits = 0, rollbacks = 0;
    bool begin_ok = true, commit_ok = true, rollback_ok = true;
    bool throw_query = false, throw_account = false, throw_write = false, throw_rollback = false;
    std::shared_ptr<pr::IDBResult> query(const std::string& sql) override { return query(sql, {}); }
    std::shared_ptr<pr::IDBResult> query(const std::string& sql, const std::vector<std::string>& parameters) override {
        queries.push_back({sql, parameters});
        if (throw_query || (throw_account && sql.find("FROM users") != std::string::npos)) throw std::runtime_error("query failure");
        return pop_sync_result();
    }
    bool execute(const std::string& sql) override { return execute(sql, {}); }
    bool execute(const std::string& sql, const std::vector<std::string>& parameters) override {
        writes.push_back({sql, parameters});
        if (throw_write) throw std::runtime_error("write failure");
        return test::MockDbConnection::execute(sql, parameters);
    }
    bool begin_transaction() override { ++begins; return begin_ok; }
    bool commit() override { ++commits; return commit_ok; }
    bool rollback() override {
        ++rollbacks;
        if (throw_rollback) throw std::runtime_error("rollback failure");
        return rollback_ok;
    }
};
struct OneSlotPool final : test::MockDbPool {
    std::shared_ptr<Connection> db = std::make_shared<Connection>();
    int active = 0, borrows = 0, returns = 0, blocked = 0;
    std::shared_ptr<pr::IDBConnection> get_connection() override {
        if (active) { ++blocked; return {}; }
        ++active; ++borrows; return db;
    }
    void release_connection(std::shared_ptr<pr::IDBConnection>) override {
        assert(active == 1); --active; ++returns;
    }
};
struct Fixture {
    std::shared_ptr<OneSlotPool> pool = std::make_shared<OneSlotPool>();
    std::shared_ptr<router::StaticShardRouter> router = std::make_shared<router::StaticShardRouter>(
        std::vector<std::pair<std::string, int>>{}, 0,
        std::vector<std::shared_ptr<pr::DBPool>>{pool},
        std::vector<std::shared_ptr<pr::storage::IStorageProvider>>{});
};
webmail::MailResourceScope scope() { return {42, "user@test.local", 7, 9}; }
std::shared_ptr<test::MockDbResult> unique_index(const std::string& unique = "0") {
    return rows({{{"Non_unique", unique}, {"Seq_in_index", "1"}, {"Column_name", "account_shard"}},
                 {{"Non_unique", unique}, {"Seq_in_index", "2"}, {"Column_name", "user_id"}}});
}
const std::string token(64, 'a');
webmail::WebSessionInfo session_user() { return {42, "user@test.local", credential_stamp("current-password"), 0}; }
std::shared_ptr<test::MockDbResult> registry(const std::string& shard = "0") {
    return rows({{{"user_id", "42"}, {"account_address", "user@test.local"}, {"account_shard", shard},
                 {"credential_stamp", credential_stamp("current-password")}}});
}
std::shared_ptr<test::MockDbResult> account(const std::string& password = "current-password",
                                         const std::string& status = "1", const std::string& id = "42") {
    return rows({{{"id", id}, {"status", status}, {"password", password}}});
}

void flag_scope_transaction_and_sent_rules() {
    Fixture f;
    auto repo = repository::make_sql_webmail_action_repository(f.router);
    auto db = f.pool->db;
    db->push_sync_result(rows({{{"box_type", "1"}}}));
    const auto result = repo->update_flags({scope(), true, false});
    assert(!result.server_error && !result.not_found);
    assert(db->begins == 1 && db->commits == 1 && db->rollbacks == 0);
    assert(db->queries[0].sql.find("FOR UPDATE") != std::string::npos);
    assert((db->queries[0].parameters == std::vector<std::string>{"9", "7", "42", "42"}));
    assert((db->writes[0].parameters == std::vector<std::string>{"0", "9", "user@test.local"}));
    assert((db->writes[1].parameters == std::vector<std::string>{"0", "9", "7", "42"}));
    assert(f.pool->active == 0 && f.pool->returns == 1);
    db->writes.clear();
    db->push_sync_result(rows({{{"box_type", "2"}}}));
    auto sent = repo->update_flags({scope(), false, true});
    assert(!sent.server_error && !sent.not_found && db->writes.size() == 1);
    assert(db->writes[0].sql.find("is_starred") != std::string::npos);
    assert(db->commits == 2 && f.pool->returns == 2);
}
void flag_failures_rollback_and_fail_closed() {
    for (int failure = 0; failure < 7; ++failure) {
        Fixture f;
        auto repo = repository::make_sql_webmail_action_repository(f.router);
        auto db = f.pool->db;
        if (failure == 0) db->begin_ok = false;
        if (failure == 1) db->push_sync_result(nullptr);
        if (failure >= 2) db->push_sync_result(rows({{{"box_type", "1"}}}));
        if (failure == 2) db->fail_next_execute();
        if (failure == 3) { db->push_execute_result(true); db->push_execute_result(false); }
        if (failure == 4) db->commit_ok = false;
        if (failure == 5) db->throw_query = true;
        if (failure == 6) { db->throw_write = true; db->throw_rollback = true; }
        const auto result = repo->update_flags({scope(), true, true});
        assert(result.server_error && !result.not_found && f.pool->active == 0 && f.pool->returns == 1);
        assert(db->rollbacks == (failure == 0 ? 0 : 1));
        if (failure == 6) assert(!db->is_connected());
    }
    for (bool rollback_ok : {false, true}) {
        Fixture f;
        auto repo = repository::make_sql_webmail_action_repository(f.router);
        f.pool->db->rollback_ok = rollback_ok;
        const auto result = repo->update_flags({scope(), true, true});
        assert(result.not_found == rollback_ok && result.server_error == !rollback_ok);
        assert(f.pool->db->writes.empty() && f.pool->db->commits == 0 && f.pool->db->rollbacks == 1);
        assert(f.pool->db->is_connected() == rollback_ok);
    }
    auto unavailable = repository::make_sql_webmail_action_repository(nullptr);
    assert(unavailable->update_flags({scope(), true, false}).server_error);
}
void attachment_authorization_metadata() {
    Fixture f;
    auto repo = repository::make_sql_webmail_action_repository(f.router);
    f.pool->db->push_sync_result(rows({{{"filepath", "opaque/object"}, {"filename", "file.bin"}}}));
    auto result = repo->find_attachment(scope(), 11);
    assert(!result.server_error && !result.not_found && result.shard == 0);
    assert(result.object_key == "opaque/object" && result.filename == "file.bin");
    assert((f.pool->db->queries[0].parameters == std::vector<std::string>{"11", "9", "7", "42", "42"}));
    assert(f.pool->active == 0 && f.pool->returns == 1);
    assert(repo->find_attachment(scope(), 11).not_found);
    f.pool->db->push_sync_result(nullptr);
    assert(repo->find_attachment(scope(), 11).server_error);
    f.pool->db->throw_query = true;
    assert(repo->find_attachment(scope(), 11).server_error && f.pool->active == 0);
}
void session_put_schema_hash_and_single_account_upsert() {
    for (auto schema : std::vector<std::shared_ptr<pr::IDBResult>>{nullptr, rows(), unique_index("1"),
        rows({{{"Non_unique", "0"}, {"Seq_in_index", "1"}, {"Column_name", "user_id"}},
              {{"Non_unique", "0"}, {"Seq_in_index", "2"}, {"Column_name", "account_shard"}}})}) {
        Fixture f;
        f.pool->db->push_sync_result(schema);
        auto repo = repository::make_sql_web_session_repository(f.router);
        assert(!repo->put(token, session_user(), std::chrono::seconds(60)));
        assert(f.pool->db->writes.empty() && f.pool->returns == 1);
    }
    Fixture f;
    auto repo = repository::make_sql_web_session_repository(f.router);
    assert(!repo->put("invalid", session_user(), std::chrono::seconds(60)) && !f.pool->borrows);
    auto no_stamp = session_user(); no_stamp.credential_stamp.clear();
    assert(!repo->put(token, no_stamp, std::chrono::seconds(60)) && !f.pool->borrows);
    f.pool->db->push_sync_result(unique_index());
    f.pool->db->push_execute_result(true); f.pool->db->push_execute_result(false); // cleanup failure is tolerated
    assert(repo->put(token, session_user(), std::chrono::seconds(3600)));
    const auto& insertion = f.pool->db->writes[0];
    assert(insertion.sql.find("ON DUPLICATE KEY UPDATE") != std::string::npos);
    assert(insertion.sql.find("token_hash=IF(account_shard=VALUES(account_shard)") != std::string::npos);
    assert((insertion.parameters == std::vector<std::string>{credential_stamp(token), "42", "user@test.local", "0",
                                                            credential_stamp("current-password"), "3600"}));
    assert(insertion.sql.find(token) == std::string::npos && insertion.parameters[0] != token);
    assert(f.pool->db->writes.size() == 2 && f.pool->active == 0);
    f.pool->db->push_sync_result(unique_index()); f.pool->db->fail_next_execute();
    assert(!repo->put(token, session_user(), std::chrono::seconds(60)) && f.pool->db->writes.size() == 3);
}
void lookup_releases_registry_before_fresh_account() {
    Fixture f;
    auto repo = repository::make_sql_web_session_repository(f.router);
    for (bool changed : {false, true}) {
        f.pool->db->push_sync_result(registry());
        f.pool->db->push_sync_result(account(changed ? "replacement-password" : "current-password"));
        const auto result = repo->lookup(token);
        assert(result.found == !changed && !result.server_error && result.user.user_id == 42);
        assert(result.user.credential_stamp == credential_stamp("current-password"));
    }
    assert(f.pool->borrows == 4 && f.pool->returns == 4 && !f.pool->blocked && !f.pool->active);
    const auto& registry_query = f.pool->db->queries[0];
    assert(registry_query.sql.find("expires_at>UTC_TIMESTAMP()") != std::string::npos);
    assert((registry_query.parameters == std::vector<std::string>{credential_stamp(token)}));
    assert((f.pool->db->queries[1].parameters == std::vector<std::string>{"user@test.local"}));
}
void session_lookup_failures_and_revocation() {
    for (int failure = 0; failure < 7; ++failure) {
        Fixture f;
        auto repo = repository::make_sql_web_session_repository(f.router);
        if (failure == 0) f.pool->db->push_sync_result(nullptr);
        else f.pool->db->push_sync_result(registry(failure == 1 ? "99" : "0"));
        if (failure == 2) f.pool->db->push_sync_result(nullptr);
        if (failure == 3) f.pool->db->push_sync_result(account("current-password", "0"));
        if (failure == 4) f.pool->db->push_sync_result(account("current-password", "1", "43"));
        if (failure == 5) f.pool->db->push_sync_result(rows());
        if (failure == 6) f.pool->db->throw_account = true;
        auto result = repo->lookup(token);
        assert(!result.found && result.server_error == (failure <= 2 || failure == 6));
        assert(!f.pool->active && !f.pool->blocked && f.pool->borrows == f.pool->returns);
    }
    Fixture f;
    auto repo = repository::make_sql_web_session_repository(f.router);
    auto missing = repo->lookup(token); // expired, replaced, revoked or unknown token: no registry row
    assert(!missing.found && !missing.server_error && f.pool->borrows == 1);
    const auto borrowed = f.pool->borrows;
    assert(!repo->lookup("malformed").found && repo->erase("malformed") && f.pool->borrows == borrowed);
    assert(repo->erase(token));
    assert((f.pool->db->writes.back().parameters == std::vector<std::string>{credential_stamp(token)}));
    assert(f.pool->db->writes.back().sql == "DELETE FROM web_sessions WHERE token_hash=?");
    f.pool->db->fail_next_execute(); assert(!repo->erase(token));
    auto unavailable = repository::make_sql_web_session_repository(nullptr);
    assert(!unavailable->put(token, session_user(), std::chrono::seconds(60)));
    assert(unavailable->lookup(token).server_error && !unavailable->erase(token));
}
} // namespace
int main() {
    flag_scope_transaction_and_sent_rules();
    flag_failures_rollback_and_fail_closed();
    attachment_authorization_metadata();
    session_put_schema_hash_and_single_account_upsert();
    lookup_releases_registry_before_fresh_account();
    session_lookup_failures_and_revocation();
    std::cout << "Webmail persistence repository contracts passed\n";
}
