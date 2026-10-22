#undef NDEBUG
#include "mail_system/back/repository/pop3_mailbox_repository.h"
#include "mail_system/back/repository/pop3_lock_repository.h"
#include "mock_db_pool.h"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>

#if defined(BOOST_ASIO_VERSION) || defined(BOOST_VERSION)
#error "POP3 repository contracts must not expose an Asio backend"
#endif
namespace {
using namespace mail_system;
using namespace mail_system::repository;
std::shared_ptr<test::MockDbResult> rows(std::vector<std::map<std::string, std::string>> values = {}) {
    return std::make_shared<test::MockDbResult>(std::move(values));
}
struct TrackingPool final : test::MockDbPool {
    explicit TrackingPool(std::shared_ptr<std::vector<std::string>> e) : events(std::move(e)) {}
    ~TrackingPool() override { events->push_back("destroy"); }
    void release_connection(std::shared_ptr<pr::IDBConnection>) override { events->push_back("return"); }
    std::shared_ptr<std::vector<std::string>> events;
};
void mailbox_reads_preserve_scope_and_rows() {
    auto repository = make_sql_pop3_mailbox_repository();
    auto pool = std::make_shared<test::MockDbPool>(); auto db = pool->mock_conn();
    auto context = make_pop3_db_context(pool); int calls = 0;
    repository->get_inbox_async(context, 41, [&](bool ok, uint64_t id) { assert(ok && id == 7); ++calls; });
    assert((db->last_query_params() == std::vector<std::string>{"41"}));
    assert(db->last_query_sql().find("box_type = 1") != std::string::npos);
    db->fire_query(rows({{{"id", "7"}}}));
    repository->get_inbox_async(context, 41, [&](bool ok, uint64_t id) { assert(ok && !id); ++calls; });
    db->fire_query(rows());
    repository->get_inbox_async(context, 41, [&](bool ok, uint64_t id) { assert(!ok && !id); ++calls; });
    db->fire_query(nullptr);
    repository->get_messages_async(context, 41, 7, [&](bool ok, std::vector<MailboxMailInfo> messages) {
        assert(ok && messages.size() == 2 && messages[0].mail_id == 51 && messages[0].body_path == "opaque/51");
        assert(messages[0].is_deleted && messages[1].mail_id == 52); ++calls;
    });
    assert((db->last_query_params() == std::vector<std::string>{"7", "41"}));
    assert(db->last_query_sql().find("is_deleted = 0") == std::string::npos);
    db->fire_query(rows({{{"id", "51"}, {"body_path", "opaque/51"}, {"is_deleted", "1"}}, {{"id", "52"}}}));
    repository->get_messages_async(context, 41, 7, [&](bool ok, std::vector<MailboxMailInfo> messages) { assert(ok && messages.empty()); ++calls; });
    db->fire_query(rows());
    repository->get_messages_async(context, 41, 7, [&](bool ok, std::vector<MailboxMailInfo> messages) { assert(!ok && messages.empty()); ++calls; });
    db->fire_query(nullptr);
    repository->get_inbox_async({}, 41, [&](bool ok, uint64_t id) { assert(!ok && !id); ++calls; });
    assert(calls == 7);
}
void lock_chains_stop_after_write_failure() {
    auto repository = make_sql_pop3_lock_repository();
    auto pool = std::make_shared<test::MockDbPool>(); auto db = pool->mock_conn();
    auto context = make_pop3_db_context(pool); int calls = 0;
    for (bool renew : {false, true}) {
        auto complete = [&](Pop3LockStatus status) { assert(status == Pop3LockStatus::database_error); ++calls; };
        if (renew) repository->renew_async(context, {41, "session-A"}, complete);
        else repository->acquire_async(context, {41, "session-A"}, complete);
        assert((db->last_query_params() == std::vector<std::string>{"41", "session-A"}));
        db->fire_execute(false);
        assert(!db->has_pending_query() && !db->has_pending_execute());
    }
    assert(calls == 2);
    for (bool renew : {false, true}) {
        for (const auto& result : std::vector<std::shared_ptr<pr::IDBResult>>{
                rows({{{"cnt", "1"}}}), rows({{{"cnt", "0"}}}), nullptr, rows(), rows({{{"cnt", "junk"}}})}) {
            Pop3LockStatus output = Pop3LockStatus::owned;
            auto complete = [&](Pop3LockStatus status) { output = status; ++calls; };
            if (renew) repository->renew_async(context, {41, "session-A"}, complete);
            else repository->acquire_async(context, {41, "session-A"}, complete);
            assert(!db->has_pending_query()); db->fire_execute(true);
            assert(db->has_pending_query() && !db->has_pending_execute());
            assert((db->last_query_params() == std::vector<std::string>{"41", "session-A"}));
            db->fire_query(result);
            const auto expected = result && result->get_row_count() && result->get_value(0, "cnt") == "1" ? Pop3LockStatus::owned
                : result && result->get_row_count() && result->get_value(0, "cnt") == "0" ? Pop3LockStatus::not_owned : Pop3LockStatus::database_error;
            assert(output == expected);
        }
    }
    repository->verify_async(context, {41, "session-B"}, [&](Pop3LockStatus status) { assert(status == Pop3LockStatus::not_owned); ++calls; });
    assert((db->last_query_params() == std::vector<std::string>{"41", "session-B"}));
    db->fire_query(rows({{{"cnt", "0"}}}));
    repository->release_async(context, {41, "session-B"}, [&](bool ok) { assert(!ok); ++calls; });
    assert(db->last_query_sql() == "DELETE FROM pop3_session_lock WHERE user_id = ? AND session_id = ?");
    assert((db->last_query_params() == std::vector<std::string>{"41", "session-B"}));
    db->fire_execute(false);
    assert(repository->sweep_expired(context));
    db->fail_next_execute(); assert(!repository->sweep_expired(context));
    assert(!repository->sweep_expired({}) && calls == 14);
}
void deletion_failures_and_empty_sets() {
    auto repository = make_sql_pop3_mailbox_repository();
    auto pool = std::make_shared<test::MockDbPool>(); auto db = pool->mock_conn();
    auto context = make_pop3_db_context(pool); int calls = 0;
    repository->apply_deletions_async({}, {41, 7, {}}, [&](Pop3DeletionResult result) { assert(result.ok && !result.may_have_changed); ++calls; });
    assert(db->last_query_sql().empty());
    repository->apply_deletions_async({}, {41, 7, {51}}, [&](Pop3DeletionResult result) { assert(!result.ok && !result.may_have_changed); ++calls; });
    repository->apply_deletions_async(context, {41, 7, {51, 52}}, [&](Pop3DeletionResult result) { assert(!result.ok && result.may_have_changed); ++calls; });
    assert(db->last_query_sql().find("mail_id IN (51,52)") != std::string::npos);
    assert(db->last_query_sql().find("AND user_id = 41 AND mailbox_id = 7") != std::string::npos);
    db->fire_execute(false);
    assert(!db->has_pending_execute() && calls == 3); // Failed mark never expunges.
    for (bool success : {false, true}) {
        repository->apply_deletions_async(context, {41, 7, {51}}, [&](Pop3DeletionResult result) { assert(result.ok == success && result.may_have_changed); ++calls; });
        db->fire_execute(true);
        assert(db->last_query_sql() == "DELETE FROM mail_mailbox WHERE mailbox_id = ? AND user_id = ? AND is_deleted = 1");
        assert((db->last_query_params() == std::vector<std::string>{"7", "41"}));
        db->fire_execute(success);
    }
    assert(calls == 5);
}
void pool_and_lease_survive_every_delayed_stage() {
    auto repository = make_sql_pop3_lock_repository();
    auto events = std::make_shared<std::vector<std::string>>();
    auto pool = std::make_shared<TrackingPool>(events); auto db = pool->mock_conn();
    std::weak_ptr<TrackingPool> weak = pool; bool called = false;
    repository->renew_async(make_pop3_db_context(pool), {41, "session"}, [&](Pop3LockStatus status) {
        assert(status == Pop3LockStatus::owned && !weak.expired() && events->empty()); called = true;
    });
    pool.reset(); assert(!weak.expired() && events->empty());
    db->fire_execute(true); assert(!weak.expired() && events->empty() && !called);
    db->fire_query(rows({{{"cnt", "1"}}}));
    assert(called && weak.expired() && (*events == std::vector<std::string>{"return", "destroy"}));
    // The read callback must retain the pool through result mapping too.
    auto mailboxes = make_sql_pop3_mailbox_repository(); events->clear();
    pool = std::make_shared<TrackingPool>(events); db = pool->mock_conn(); weak = pool;
    mailboxes->get_messages_async(make_pop3_db_context(pool), 41, 7, [&](bool ok, std::vector<MailboxMailInfo>) {
        assert(ok && !weak.expired() && events->empty());
    });
    pool.reset(); assert(!weak.expired()); db->fire_query(rows());
    assert(weak.expired() && (*events == std::vector<std::string>{"return", "destroy"}));
    // Inline drivers keep completion shape unchanged and do not retain a cycle.
    events->clear(); pool = std::make_shared<TrackingPool>(events); db = pool->mock_conn(); db->set_deferred(false);
    db->push_sync_result(rows({{{"cnt", "1"}}})); called = false;
    repository->acquire_async(make_pop3_db_context(pool), {41, "inline"}, [&](Pop3LockStatus status) { assert(status == Pop3LockStatus::owned); called = true; });
    assert(called && *events == std::vector<std::string>{"return"});
    pool.reset(); assert((*events == std::vector<std::string>{"return", "destroy"}));
}
class ThrowingResult final : public test::MockDbResult {
public:
    // 显式写出参数类型：gcc-11 下裸花括号列表会同时匹配 vector 转换构造与
    // 拷贝/移动构造（ambiguous），clang 与 gcc-13 均接受，只有 gcc-11 拒绝。
    ThrowingResult()
        : MockDbResult(std::vector<std::map<std::string, std::string>>(
              {{{"id", "1"}, {"cnt", "1"}}})) {}
    std::string get_value(size_t, const std::string&) const override { throw std::runtime_error("column mapping failed"); }
};
class ThrowingConnection final : public test::MockDbConnection {
public:
    bool throw_query = false, throw_write = false, throw_after_write = false, throw_sweep = false;
    void async_query(const std::string& sql, pr::QueryCallback cb) override {
        if (throw_query) throw std::runtime_error("query start failed");
        MockDbConnection::async_query(sql, std::move(cb));
    }
    void async_query(const std::string& sql, const std::vector<std::string>& params, pr::QueryCallback cb) override {
        if (throw_query) throw std::runtime_error("query start failed");
        MockDbConnection::async_query(sql, params, std::move(cb));
    }
    void async_execute(const std::string& sql, pr::ExecuteCallback cb) override {
        if (throw_write) throw std::runtime_error("write start failed");
        MockDbConnection::async_execute(sql, std::move(cb));
        if (throw_after_write) throw std::runtime_error("driver threw after inline completion");
    }
    void async_execute(const std::string& sql, const std::vector<std::string>& params, pr::ExecuteCallback cb) override {
        if (throw_write) throw std::runtime_error("write start failed");
        MockDbConnection::async_execute(sql, params, std::move(cb));
        if (throw_after_write) throw std::runtime_error("driver threw after inline completion");
    }
    bool execute(const std::string& sql) override {
        if (throw_sweep) throw std::runtime_error("sweep failed");
        return MockDbConnection::execute(sql);
    }
};
class ThrowingPool final : public test::MockDbPool {
public:
    std::shared_ptr<ThrowingConnection> db = std::make_shared<ThrowingConnection>();
    std::shared_ptr<pr::IDBConnection> get_connection() override { return db; }
};
void driver_and_mapping_exceptions_are_domain_failures() {
    auto pool = std::make_shared<ThrowingPool>(); auto context = make_pop3_db_context(pool); auto db = pool->db;
    auto locks = make_sql_pop3_lock_repository(); auto mailboxes = make_sql_pop3_mailbox_repository(); int calls = 0;
    for (bool renew : {false, true}) {
        auto complete = [&](Pop3LockStatus status) { assert(status == Pop3LockStatus::database_error); ++calls; };
        if (renew) locks->renew_async(context, {41, "sid"}, complete);
        else locks->acquire_async(context, {41, "sid"}, complete);
        db->throw_query = true; db->fire_execute(true); db->throw_query = false;
        assert(!db->has_pending_query()); // Delayed write -> throwing verify.
    }
    mailboxes->apply_deletions_async(context, {41, 7, {51}}, [&](Pop3DeletionResult result) { assert(!result.ok); ++calls; });
    db->throw_write = true; db->fire_execute(true); db->throw_write = false;
    assert(!db->has_pending_execute()); // Delayed mark -> throwing expunge.
    mailboxes->get_inbox_async(context, 41, [&](bool ok, uint64_t id) { assert(!ok && !id); ++calls; });
    db->fire_query(std::make_shared<ThrowingResult>());
    mailboxes->get_messages_async(context, 41, 7, [&](bool ok, std::vector<MailboxMailInfo> messages) { assert(!ok && messages.empty()); ++calls; });
    db->fire_query(std::make_shared<ThrowingResult>());
    locks->verify_async(context, {41, "sid"}, [&](Pop3LockStatus status) { assert(status == Pop3LockStatus::database_error); ++calls; });
    db->fire_query(std::make_shared<ThrowingResult>());
    db->throw_write = true;
    locks->release_async(context, {41, "sid"}, [&](bool ok) { assert(!ok); ++calls; });
    locks->acquire_async(context, {41, "sid"}, [&](Pop3LockStatus status) { assert(status == Pop3LockStatus::database_error); ++calls; });
    mailboxes->apply_deletions_async(context, {41, 7, {51}}, [&](Pop3DeletionResult result) { assert(!result.ok); ++calls; });
    db->throw_write = false; db->throw_query = true;
    mailboxes->get_inbox_async(context, 41, [&](bool ok, uint64_t) { assert(!ok); ++calls; });
    mailboxes->get_messages_async(context, 41, 7, [&](bool ok, std::vector<MailboxMailInfo>) { assert(!ok); ++calls; });
    db->throw_query = false; db->throw_sweep = true; assert(!locks->sweep_expired(context));
    assert(calls == 11);
    db->set_deferred(false); db->throw_after_write = true;
    db->push_sync_result(rows({{{"cnt", "1"}}})); int once = 0;
    locks->acquire_async(context, {41, "inline"}, [&](Pop3LockStatus status) { assert(status == Pop3LockStatus::owned); ++once; });
    assert(once == 1); // Do not send failure after an already delivered success.
}

} // namespace
int main() {
    mailbox_reads_preserve_scope_and_rows(); lock_chains_stop_after_write_failure();
    deletion_failures_and_empty_sets(); pool_and_lease_survive_every_delayed_stage();
    driver_and_mapping_exceptions_are_domain_failures();
    std::cout << "POP3 repositories passed\n";
}
