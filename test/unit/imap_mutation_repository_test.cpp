#undef NDEBUG
#include "mail_system/back/repository/imap_mutation_repository.h"
#include "mock_db_pool.h"
#include <cassert>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <utility>

#if defined(BOOST_ASIO_VERSION) || defined(BOOST_VERSION)
#error "IMAP mutation repository must not expose a network backend"
#endif

namespace {
using namespace mail_system;
using namespace mail_system::repository;
using test::MockDbResult;
struct TrackingPool final : test::MockDbPool {
    int returns = 0;
    void release_connection(std::shared_ptr<pr::IDBConnection>) override { ++returns; }
};
std::shared_ptr<MockDbResult> rows(std::vector<std::map<std::string, std::string>> value = {}) {
    return std::make_shared<MockDbResult>(std::move(value));
}
ImapStoreRequest store_request() {
    ImapStoreRequest request;
    request.scope = {41, 7};
    request.mail_ids = {5, 9};
    request.recipient_fallback = "fallback@test";
    request.seen = true;
    request.flagged = false;
    request.deleted = true;
    return request;
}
ImapCopyMoveRequest copy_request(bool move = true) {
    return {{41, 7}, 11, {5, 9}, move};
}
void store_serializes_attributes_and_retains_lease() {
    auto repository = make_sql_imap_mutation_repository();
    TrackingPool pool;
    auto db = pool.mock_conn();
    int calls = 0;
    repository->store_async(pool.acquire_connection(), store_request(), [&](ImapMutationResult result) {
        assert(!result.ok && pool.returns == 0);
        ++calls;
    });
    assert(!calls && pool.returns == 0 && db->has_pending_query() && !db->has_pending_execute());
    assert((db->last_query_params() == std::vector<std::string>{"41"}));
    db->fire_query(rows({{{"mail_address", "a'b@test"}}}));
    assert(!db->has_pending_query() && db->has_pending_execute());
    assert(db->last_query_sql().find("IN (5,9)") != std::string::npos);
    assert(db->last_query_sql().find("a'b@test") == std::string::npos);
    assert((db->last_query_params() == std::vector<std::string>{"0", "a'b@test"}));
    db->fire_execute(true);
    assert(db->last_query_sql().find("is_starred = ?") != std::string::npos);
    assert((db->last_query_params() == std::vector<std::string>{"0", "41", "7"}));
    db->fire_execute(false);
    assert(db->last_query_sql().find("is_deleted = ?") != std::string::npos);
    assert((db->last_query_params() == std::vector<std::string>{"1", "41", "7"}));
    assert(!calls && !pool.returns);
    db->fire_execute(true);
    assert(calls == 1 && pool.returns == 1 && !db->has_pending_execute());
}
void store_lookup_failure_uses_authenticated_fallback() {
    auto repository = make_sql_imap_mutation_repository();
    TrackingPool pool;
    auto db = pool.mock_conn();
    auto request = store_request();
    request.seen = false;
    request.flagged.reset(); request.deleted.reset();
    bool done = false;
    repository->store_async(pool.acquire_connection(), request, [&](ImapMutationResult result) {
        done = true; assert(result.ok);
    });
    db->fire_query(nullptr);
    assert((db->last_query_params() == std::vector<std::string>{"1", "fallback@test"}));
    db->fire_execute(true);
    assert(done && pool.returns == 1);
}
void failed_insert_never_marks_move_source() {
    auto repository = make_sql_imap_mutation_repository();
    TrackingPool pool;
    auto db = pool.mock_conn();
    int calls = 0;
    repository->copy_move_async(pool.acquire_connection(), copy_request(), [&](ImapCopyMoveResult result) {
        assert(!result.copy_ok && !result.source_marked && !result.copied);
        ++calls;
    });
    assert(db->last_query_sql().find("(5,11,41,0,0,0),(9,11,41,0,0,0)") != std::string::npos);
    db->fire_execute(false);
    assert(calls == 1 && pool.returns == 1 && !db->has_pending_query() && !db->has_pending_execute());
    assert(db->last_query_sql().find("INSERT IGNORE") == 0);
}
void failed_count_never_marks_move_source() {
    for (auto result : std::vector<std::shared_ptr<pr::IDBResult>>{
            nullptr, rows(), rows({{{"affected", ""}}}), rows({{{"affected", "-1"}}}),
            rows({{{"affected", "broken"}}}), rows({{{"affected", "18446744073709551616"}}})}) {
        auto repository = make_sql_imap_mutation_repository();
        TrackingPool pool;
        auto db = pool.mock_conn();
        int calls = 0;
        repository->copy_move_async(pool.acquire_connection(), copy_request(), [&](ImapCopyMoveResult value) {
            assert(!value.copy_ok && !value.source_marked && !value.copied);
            ++calls;
        });
        db->fire_execute(true);
        assert(db->has_pending_query() && !db->has_pending_execute() && !pool.returns);
        db->fire_query(result);
        assert(calls == 1 && pool.returns == 1 && !db->has_pending_execute());
    }
}
void successful_zero_count_moves_and_reports_source_failure() {
    for (bool mark_ok : {false, true}) {
        auto repository = make_sql_imap_mutation_repository();
        TrackingPool pool;
        auto db = pool.mock_conn();
        int calls = 0;
        repository->copy_move_async(pool.acquire_connection(), copy_request(), [&](ImapCopyMoveResult result) {
            assert(result.copy_ok && result.copied == 0 && result.source_marked == mark_ok);
            assert(pool.returns == 0);
            ++calls;
        });
        db->fire_execute(true);
        db->fire_query(rows({{{"affected", "0"}}}));
        assert(!calls && !pool.returns && db->has_pending_execute());
        assert(db->last_query_sql().find("is_deleted = 1 WHERE mail_id IN (5,9)") != std::string::npos);
        assert((db->last_query_params() == std::vector<std::string>{"41", "7"}));
        db->fire_execute(mark_ok);
        assert(calls == 1 && pool.returns == 1);
    }
}
void same_mailbox_move_rejected_but_copy_is_safe() {
    auto repository = make_sql_imap_mutation_repository();
    TrackingPool pool;
    auto db = pool.mock_conn();
    auto request = copy_request();
    request.target_mailbox_id = request.source.mailbox_id;
    int calls = 0;
    repository->copy_move_async(pool.acquire_connection(), request, [&](ImapCopyMoveResult result) {
        assert(!result.copy_ok && !result.source_marked); ++calls;
    });
    assert(calls == 1 && pool.returns == 1 && db->last_query_sql().empty());
    assert(!db->has_pending_query() && !db->has_pending_execute());
    request.move = false;
    repository->copy_move_async(pool.acquire_connection(), request, [&](ImapCopyMoveResult result) {
        assert(result.copy_ok && result.copied == 0 && !result.source_marked); ++calls;
    });
    db->fire_execute(true);
    db->fire_query(rows({{{"affected", "0"}}}));
    assert(calls == 2 && pool.returns == 2 && !db->has_pending_execute());
}
void copy_count_and_expunge_scope() {
    auto repository = make_sql_imap_mutation_repository();
    TrackingPool pool;
    auto db = pool.mock_conn();
    int calls = 0;
    repository->copy_move_async(pool.acquire_connection(), copy_request(false), [&](ImapCopyMoveResult result) {
        assert(result.copy_ok && result.copied == 2 && !result.source_marked);
        ++calls;
    });
    db->fire_execute(true);
    db->fire_query(rows({{{"affected", "2"}}}));
    assert(calls == 1 && pool.returns == 1 && !db->has_pending_execute());
    repository->expunge_async(pool.acquire_connection(), {41, 7}, [&](ImapMutationResult result) {
        assert(!result.ok && pool.returns == 1); ++calls;
    });
    assert((db->last_query_params() == std::vector<std::string>{"7", "41"}));
    assert(db->last_query_sql().find("AND is_deleted = 1") != std::string::npos);
    db->fire_execute(false);
    assert(calls == 2 && pool.returns == 2);
}
void empty_invalid_and_inline_completion() {
    auto repository = make_sql_imap_mutation_repository();
    int calls = 0;
    repository->store_async({}, store_request(), [&](ImapMutationResult result) { assert(!result.ok); ++calls; });
    repository->expunge_async({}, {41, 7}, [&](ImapMutationResult result) { assert(!result.ok); ++calls; });
    repository->copy_move_async({}, copy_request(), [&](ImapCopyMoveResult result) { assert(!result.copy_ok); ++calls; });
    TrackingPool pool;
    auto db = pool.mock_conn();
    auto empty_store = store_request(); empty_store.mail_ids.clear();
    repository->store_async(pool.acquire_connection(), empty_store, [&](ImapMutationResult result) { assert(result.ok); ++calls; });
    auto empty_copy = copy_request(); empty_copy.mail_ids.clear();
    repository->copy_move_async(pool.acquire_connection(), empty_copy, [&](ImapCopyMoveResult result) {
        assert(result.copy_ok && result.copied == 0); ++calls;
    });
    assert(calls == 5 && pool.returns == 2 && !db->has_pending_query() && !db->has_pending_execute());
    db->set_deferred(false);
    db->push_sync_result(rows({{{"mail_address", "inline@test"}}}));
    repository->store_async(pool.acquire_connection(), store_request(), [&](ImapMutationResult result) { assert(result.ok); ++calls; });
    assert(calls == 6 && pool.returns == 3);
    db->push_sync_result(rows({{{"affected", "0"}}}));
    repository->copy_move_async(pool.acquire_connection(), copy_request(), [&](ImapCopyMoveResult result) {
        assert(result.copy_ok && result.source_marked && !result.copied); ++calls;
    });
    assert(calls == 7 && pool.returns == 4);
}
} // namespace
int main() {
    store_serializes_attributes_and_retains_lease();
    store_lookup_failure_uses_authenticated_fallback();
    failed_insert_never_marks_move_source();
    failed_count_never_marks_move_source();
    successful_zero_count_moves_and_reports_source_failure();
    same_mailbox_move_rejected_but_copy_is_safe();
    copy_count_and_expunge_scope();
    empty_invalid_and_inline_completion();
    std::cout << "IMAP mutation repository contracts passed\n";
}
