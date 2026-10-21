#undef NDEBUG
#include "mail_system/back/repository/persistence_repository.h"
#if defined(BOOST_VERSION) || defined(MYSQL_VERSION_ID)
#error "Persistence contracts must not expose concrete queue/database SDK headers"
#endif
#include "mail_system/back/router/static_shard_router.h"
#include "mock_db_pool.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace mail_system;
using namespace mail_system::repository;
std::shared_ptr<test::MockDbResult> rows(std::vector<std::map<std::string, std::string>> value = {}) {
    return std::make_shared<test::MockDbResult>(std::move(value));
}
struct ControlledConnection final : test::MockDbConnection {
    pr::ExecuteCallback begin_callback, commit_callback, rollback_callback;
    int begins = 0, commits = 0, rollbacks = 0, disconnects = 0, sync_writes = 0;
    int fail_sync_write = 0, throw_sync_write = 0;
    bool throw_query = false, throw_write = false, throw_rollback = false, sync_commit_ok = true, sync_rollback_ok = true;
    bool inline_transactions = false, throw_after_commit = false;
    std::vector<std::string> executed;
    bool pending() const { return begin_callback || commit_callback || rollback_callback || has_pending_query() || has_pending_execute(); }
    void async_begin_transaction(pr::ExecuteCallback cb) override {
        assert(!pending()); ++begins;
        if (inline_transactions) cb(true); else begin_callback = std::move(cb);
    }
    void async_commit(pr::ExecuteCallback cb) override {
        assert(!pending()); ++commits;
        if (inline_transactions) { cb(true); if (throw_after_commit) throw std::runtime_error("commit driver threw after completion"); }
        else commit_callback = std::move(cb);
    }
    void async_rollback(pr::ExecuteCallback cb) override {
        assert(!pending()); ++rollbacks;
        if (throw_rollback) throw std::runtime_error("rollback start failed");
        rollback_callback = std::move(cb);
    }
    void async_query(const std::string& sql, pr::QueryCallback cb) override {
        assert(!pending()); if (throw_query) throw std::runtime_error("query start failed");
        MockDbConnection::async_query(sql, std::move(cb));
    }
    void async_query(const std::string& sql, const std::vector<std::string>& params, pr::QueryCallback cb) override {
        assert(!pending()); if (throw_query) throw std::runtime_error("query start failed");
        MockDbConnection::async_query(sql, params, std::move(cb));
    }
    void async_execute(const std::string& sql, pr::ExecuteCallback cb) override {
        assert(!pending()); if (throw_write) throw std::runtime_error("write start failed");
        MockDbConnection::async_execute(sql, std::move(cb));
    }
    void async_execute(const std::string& sql, const std::vector<std::string>& params, pr::ExecuteCallback cb) override {
        assert(!pending()); if (throw_write) throw std::runtime_error("write start failed");
        MockDbConnection::async_execute(sql, params, std::move(cb));
    }
    // A moved-from std::function can retain its small callable in libc++.
    // Clear the pending slot before a continuation starts its next operation.
    void fire_begin(bool ok = true) {
        auto cb = std::exchange(begin_callback, pr::ExecuteCallback{});
        assert(cb && !pending()); cb(ok);
    }
    void fire_commit(bool ok = true) {
        auto cb = std::exchange(commit_callback, pr::ExecuteCallback{});
        assert(cb && !pending()); cb(ok);
    }
    void fire_rollback(bool ok = true) {
        auto cb = std::exchange(rollback_callback, pr::ExecuteCallback{});
        assert(cb && !pending()); cb(ok);
    }
    bool execute(const std::string& sql) override {
        assert(!pending()); executed.push_back(sql); ++sync_writes;
        if (throw_sync_write == sync_writes) throw std::runtime_error("sync write failed");
        return fail_sync_write != sync_writes;
    }
    bool begin_transaction() override { ++begins; return true; }
    bool commit() override { ++commits; return sync_commit_ok; }
    bool rollback() override { ++rollbacks; if (throw_rollback) throw std::runtime_error("sync rollback failed"); return sync_rollback_ok; }
    void disconnect() override { ++disconnects; MockDbConnection::disconnect(); }
};
struct Pool final : pr::DBPool {
    std::shared_ptr<ControlledConnection> db = std::make_shared<ControlledConnection>();
    std::shared_ptr<std::vector<std::string>> events;
    int active = 0, checkouts = 0, returns = 0;
    explicit Pool(std::shared_ptr<std::vector<std::string>> value = std::make_shared<std::vector<std::string>>()) : events(std::move(value)) {}
    ~Pool() override { events->push_back("destroy"); }
    size_t get_pool_size() const override { return 1; }
    size_t get_available_connections() const override { return active ? 0 : 1; }
    size_t get_max_pool_size() const override { return 1; }
    size_t get_active_connections() const override { return static_cast<size_t>(active); }
    void close() override {}
    std::shared_ptr<pr::IDBConnection> get_connection() override { assert(!active); ++active; ++checkouts; return db; }
    void release_connection(std::shared_ptr<pr::IDBConnection>) override { assert(active == 1 && !db->pending()); --active; ++returns; events->push_back("return"); }
protected:
    void initialize_pool() override {}
    std::shared_ptr<pr::IDBConnection> create_connection() override { return db; }
};
std::shared_ptr<router::StaticShardRouter> router_for(std::vector<std::shared_ptr<pr::DBPool>> pools) {
    return std::make_shared<router::StaticShardRouter>(std::vector<std::pair<std::string, int>>{{"local.test", 1}}, 0,
        std::move(pools), std::vector<std::shared_ptr<pr::storage::IStorageProvider>>{});
}
std::shared_ptr<::mail> message() {
    auto value = std::make_shared<::mail>(); value->id = 51; value->from = "sender@origin.test";
    value->to = {"recipient@remote.test"}; value->ids = {91}; value->subject = "probe";
    value->send_time = 10; value->body_path = "opaque/body/51"; return value;
}
struct Fixture {
    std::shared_ptr<Pool> pool = std::make_shared<Pool>();
    std::shared_ptr<ControlledConnection> db = pool->db;
    std::shared_ptr<router::StaticShardRouter> router = router_for({pool});
    std::shared_ptr<IPersistenceRepository> repository = make_sql_persistence_repository(router);
    std::shared_ptr<::mail> mail = message();
    PersistenceRequest request{mail, {"local.test", 13, {}, 0}};
    PersistenceResult output;
    int callbacks = 0;
    void start() { repository->persist_async(request, [&](PersistenceResult result) { output = std::move(result); ++callbacks; assert(pool->active == 1); }); }
    void metadata() { db->fire_begin(); db->fire_execute(true); db->fire_execute(true); }
    void outbox(uint64_t id = 501) { db->fire_execute(true); db->fire_query(rows({{{"id", std::to_string(id)}}})); }
};
void success_snapshots_and_commit_boundary() {
    Fixture f; f.mail->attachments.push_back({}); f.mail->attachments[0].filename = "probe.bin";
    f.mail->attachments[0].filepath = "opaque/attachment"; f.start();
    f.request.options.local_domain = "changed.test"; f.request.options.max_attempts = 99;
    f.metadata(); assert(f.db->last_query_sql().find("INSERT INTO attachments") == 0);
    f.db->fire_execute(true); assert(f.db->last_query_sql().find("INSERT INTO mail_outbox") == 0);
    assert(f.db->last_query_sql().find(", 0, 0, 13, NOW()") != std::string::npos);
    f.outbox(); assert(f.db->commit_callback && !f.callbacks && f.pool->active == 1);
    f.db->fire_commit();
    assert(f.callbacks == 1 && f.output.disposition == PersistenceDisposition::committed && f.output.transaction == PersistenceTransaction::committed);
    assert(f.output.outbox_records.size() == 1 && f.output.outbox_records[0].id == 501 && f.output.outbox_records[0].max_attempts == 13);
    assert(f.output.outbox_records[0].body_path == "opaque/body/51" && f.pool->returns == 1);
}
void reserved_rows_describe_the_legacy_database_values() {
    Fixture f; f.request.options.reserve_owner = "local-worker"; f.request.options.reserve_lease_seconds = 90;
    f.start(); f.metadata();
    assert(f.db->last_query_sql().find("0, 1, 8, NOW()") != std::string::npos);
    assert(f.db->last_query_sql().find("local-worker") != std::string::npos);
    f.outbox(); f.db->fire_commit();
    assert(f.output.outbox_records.size() == 1 && f.output.outbox_records[0].attempt_count == 1 && f.output.outbox_records[0].max_attempts == 8);
}
void rollback_completion_and_connection_quarantine() {
    for (int failure = 0; failure != 4; ++failure) {
        Fixture f; f.start(); f.db->fire_begin();
        f.db->throw_rollback = failure == 2;
        f.db->fire_execute(false);
        if (failure != 2) {
            assert(!f.callbacks && f.db->rollback_callback && f.pool->active == 1);
            f.db->fire_rollback(failure != 1);
        }
        assert(f.callbacks == 1 && !f.output.success() && f.output.outbox_records.empty());
        const bool rollback_ok = failure != 1 && failure != 2;
        assert(f.output.transaction == (rollback_ok ? PersistenceTransaction::rolled_back : PersistenceTransaction::rollback_failed));
        assert(f.db->disconnects == (rollback_ok ? 0 : 1) && f.pool->returns == 1);
    }
    Fixture commit; commit.start(); commit.metadata(); commit.outbox(); commit.db->fire_commit(false);
    assert(!commit.callbacks && commit.db->rollback_callback); commit.db->fire_rollback(true);
    assert(commit.callbacks == 1 && !commit.output.success() && commit.output.outbox_records.empty());
    Fixture begin; begin.start(); begin.db->fire_begin(false);
    assert(begin.callbacks == 1 && begin.db->rollbacks == 0 && begin.output.transaction == PersistenceTransaction::not_started);
}
void dedup_and_file_only_are_distinct_successes() {
    Fixture f; f.mail->source_message_id = "<source@test>"; f.mail->to.push_back("second@remote.test"); f.mail->ids.push_back(92); f.start();
    assert(f.db->last_query_sql().find("source_message_id") != std::string::npos && !f.db->begin_callback);
    f.db->fire_query(rows({{{"mail_id", "old"}}})); assert(!f.callbacks && f.db->has_pending_query());
    f.db->fire_query(rows({{{"mail_id", "old"}}}));
    assert(f.callbacks == 1 && f.output.disposition == PersistenceDisposition::deduplicated && f.db->begins == 0 && f.output.outbox_records.empty());
    Fixture miss; miss.mail->source_message_id = "<new@test>"; miss.start(); miss.db->fire_query(nullptr);
    assert(miss.db->begin_callback); miss.db->fire_begin(false); assert(!miss.output.success());
    auto repository = make_sql_persistence_repository({}); int calls = 0;
    repository->persist_async({message(), {}}, [&](PersistenceResult result) { assert(result.disposition == PersistenceDisposition::file_only && result.outbox_records.empty()); ++calls; });
    repository->persist_async({}, [&](PersistenceResult result) { assert(!result.success()); ++calls; });
    assert(calls == 2);
}
void sent_and_mapping_failures_rollback() {
    for (int failure = 0; failure != 3; ++failure) {
        Fixture f; f.mail->save_sent_copy = true; f.start(); f.metadata();
        assert((f.db->last_query_params() == std::vector<std::string>{"sender@origin.test"}));
        if (failure == 1) f.db->fire_query(nullptr);
        else {
            f.db->fire_query(rows({{{"mailbox_id", "7"}, {"user_id", "41"}}}));
            assert((f.db->last_query_params() == std::vector<std::string>{"51", "7", "41"}));
            f.db->fire_execute(failure != 2);
        }
        if (failure) { assert(f.db->rollback_callback && !f.callbacks); f.db->fire_rollback(); assert(!f.output.success()); }
        else { f.outbox(); f.db->fire_commit(); assert(f.output.success()); }
    }
    for (const auto& invalid : std::vector<std::shared_ptr<pr::IDBResult>>{nullptr, rows(), rows({{{"id", "junk"}}})}) {
        Fixture f; f.start(); f.metadata(); f.db->fire_execute(true); f.db->fire_query(invalid);
        assert(f.db->rollback_callback && !f.callbacks); f.db->fire_rollback(); assert(!f.output.success());
    }
    Fixture throwing; throwing.start(); throwing.db->fire_begin(); throwing.db->throw_write = true;
    throwing.db->fire_execute(true); assert(throwing.db->rollback_callback);
    throwing.db->fire_rollback(); assert(!throwing.output.success() && throwing.callbacks == 1);
}
class ThrowingResult final : public test::MockDbResult {
public:
    size_t get_row_count() const override { throw std::runtime_error("mapping failed"); }
};
void delayed_driver_and_mapping_exceptions_finish_once() {
    {
        Fixture f; f.start(); f.metadata(); f.db->throw_query = true; f.db->fire_execute(true);
        assert(f.db->rollback_callback && !f.callbacks); f.db->fire_rollback();
        assert(f.callbacks == 1 && !f.output.success());
    }
    for (bool sent : {false, true}) {
        Fixture f; f.mail->save_sent_copy = sent; f.start(); f.metadata();
        if (!sent) f.db->fire_execute(true);
        f.db->fire_query(std::make_shared<ThrowingResult>());
        assert(f.db->rollback_callback && !f.callbacks); f.db->fire_rollback();
        assert(f.callbacks == 1 && !f.output.success());
    }
    {
        Fixture f; f.mail->ids.clear(); f.start(); f.db->fire_begin();
        assert(f.db->rollback_callback && !f.db->has_pending_execute()); f.db->fire_rollback();
        assert(!f.output.success());
    }
    {
        Fixture f; f.mail->to = {"recipient@local.test"}; f.db->fail_sync_write = 1;
        f.start(); f.metadata(); assert(f.db->commit_callback && !f.callbacks);
        f.db->fire_commit(); assert(f.output.success() && f.output.outbox_records.empty());
    }
    {
        Fixture f; f.db->set_deferred(false); f.db->inline_transactions = true; f.db->throw_after_commit = true;
        f.db->push_sync_result(rows({{{"id", "401"}}})); f.start();
        assert(f.callbacks == 1 && f.output.disposition == PersistenceDisposition::committed && f.pool->returns == 1 && !f.db->rollbacks);
    }
}
void mail_pool_and_repository_survive_delayed_commit() {
    auto events = std::make_shared<std::vector<std::string>>(); auto pool = std::make_shared<Pool>(events); auto db = pool->db;
    std::weak_ptr<Pool> weak_pool = pool; auto value = message(); std::weak_ptr<::mail> weak_mail = value;
    auto router = router_for({pool}); auto repository = make_sql_persistence_repository(router); int calls = 0;
    repository->persist_async({value, {}}, [&](PersistenceResult result) {
        assert(result.success() && !weak_pool.expired() && !weak_mail.expired() && events->empty()); ++calls;
    });
    pool.reset(); value.reset(); router.reset(); repository.reset();
    db->fire_begin(); db->fire_execute(true); db->fire_execute(true); db->fire_execute(true);
    db->fire_query(rows({{{"id", "301"}}})); assert(!weak_pool.expired() && !calls);
    db->fire_commit();
    assert(calls == 1 && weak_pool.expired() && weak_mail.expired());
    assert((*events == std::vector<std::string>{"return", "destroy"}));
}
void recipient_transactions_are_independent_and_cleanup_is_typed() {
    for (int failure = 0; failure != 6; ++failure) {
        auto source = std::make_shared<Pool>(); auto recipient = std::make_shared<Pool>(); auto router = router_for({source, recipient});
        auto repository = make_sql_persistence_repository(router); auto value = message(); value->to = {"recipient@local.test"};
        recipient->db->fail_sync_write = failure >= 1 && failure <= 3 ? failure : 0;
        recipient->db->sync_commit_ok = failure != 4;
        recipient->db->throw_sync_write = failure == 5 ? 2 : 0;
        recipient->db->sync_rollback_ok = failure != 5;
        const auto results = repository->persist_recipient_copies({value, {"local.test", 8, {}, 0}}, 0);
        assert(results.size() == 1 && results[0].ok == !failure && results[0].shard == 1);
        assert(source->checkouts == 0 && recipient->returns == 1);
        assert(recipient->db->rollbacks == (failure ? 1 : 0));
        if (failure == 5) assert(recipient->db->disconnects == 1 && results[0].transaction == PersistenceTransaction::rollback_failed);
    }
    auto pool = std::make_shared<Pool>(); auto repository = make_sql_persistence_repository(router_for({pool}));
    assert(repository->erase_metadata({0, {}, true}).ok && !pool->checkouts);
    pool->db->fail_sync_write = 1;
    const auto result = repository->erase_metadata({0, {51, 52}, true});
    assert(!result.ok && pool->db->executed.size() == 4 && pool->returns == 1);
    assert(pool->db->executed[2].find("51,52") != std::string::npos && pool->db->executed[3].find("51,52") != std::string::npos);
}
} // namespace
int main() {
    success_snapshots_and_commit_boundary(); reserved_rows_describe_the_legacy_database_values(); rollback_completion_and_connection_quarantine();
    dedup_and_file_only_are_distinct_successes(); sent_and_mapping_failures_rollback();
    delayed_driver_and_mapping_exceptions_finish_once();
    mail_pool_and_repository_survive_delayed_commit(); recipient_transactions_are_independent_and_cleanup_is_typed();
    std::cout << "Persistence repository contracts passed\n";
}
