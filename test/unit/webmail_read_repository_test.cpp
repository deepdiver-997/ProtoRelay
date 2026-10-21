#undef NDEBUG
#include "mail_system/back/repository/webmail_read_repository.h"
#if defined(BOOST_VERSION) || defined(BOOST_ASIO_VERSION)
#error "Webmail repository API must not expose a network backend"
#endif
#include "mail_system/back/router/static_shard_router.h"
#include "mock_db_pool.h"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace mail_system;
using Rows = std::vector<std::map<std::string, std::string>>;
using test::MockDbResult;
const std::string address = "user@test.local";

std::shared_ptr<MockDbResult> rows(Rows value = {}) {
    return std::make_shared<MockDbResult>(std::move(value));
}
struct PoolState { int returns = 0; };
struct TrackingPool final : test::MockDbPool {
    std::shared_ptr<PoolState> state = std::make_shared<PoolState>();
    void release_connection(std::shared_ptr<pr::IDBConnection>) override { ++state->returns; }
};
struct Fixture {
    std::shared_ptr<TrackingPool> pool = std::make_shared<TrackingPool>();
    std::shared_ptr<router::StaticShardRouter> router = std::make_shared<router::StaticShardRouter>(
        std::vector<std::pair<std::string, int>>{{"test.local", 1}}, 0,
        std::vector<std::shared_ptr<pr::DBPool>>{nullptr, pool},
        std::vector<std::shared_ptr<pr::storage::IStorageProvider>>{});
    std::shared_ptr<repository::IWebmailReadRepository> repo = repository::make_sql_webmail_read_repository(router);
};

std::shared_ptr<MockDbResult> mailbox() {
    return rows({{{"id", "7"}, {"name", "Sent"}, {"box_type", "2"}}});
}
std::shared_ptr<MockDbResult> metadata() {
    return rows({{{"id", "9007199254740993"}, {"subject", "Hello"}, {"send_time", "123"},
        {"status", "1"}, {"is_starred", "1"}, {"is_important", "1"}, {"is_deleted", "0"},
        {"body_path", "objects/opaque-message"}}});
}

void mailbox_mapping_and_availability() {
    Fixture f;
    auto db = f.pool->mock_conn();
    int calls = 0;
    f.repo->list_mailboxes_async(42, address, [&](webmail::MailboxListResult result) {
        ++calls;
        assert(result.ok && !result.server_error && result.mailboxes.size() == 1);
        const auto& box = result.mailboxes[0];
        assert(box.id == 7 && box.name == "INBOX" && box.box_type == 1);
        assert(box.total == 12 && box.unread == 3);
        assert(f.pool->state->returns == 0); // the callback still owns its lease
    });
    assert(!calls && f.pool->state->returns == 0);
    assert((db->last_query_params() == std::vector<std::string>{address, "42"}));
    db->fire_query(rows({{{"id", "7"}, {"name", "INBOX"}, {"box_type", "1"},
                         {"total", "12"}, {"unread", "3"}}}));
    assert(calls == 1 && f.pool->state->returns == 1);
    for (bool fail : {false, true}) {
        f.repo->list_mailboxes_async(42, address, [&](webmail::MailboxListResult result) {
            ++calls;
            assert(result.ok == !fail && !result.server_error && result.mailboxes.empty());
        });
        db->fire_query(fail ? nullptr : rows());
    }
    assert(calls == 3 && f.pool->state->returns == 3);

    auto unavailable = repository::make_sql_webmail_read_repository(nullptr);
    unavailable->list_mailboxes_async(42, address, [&](webmail::MailboxListResult r) {
        ++calls; assert(!r.ok && r.server_error);
    });
    unavailable->list_messages_async(42, address, 7, 2, 10, [&](webmail::MessagePageResult r) {
        ++calls; assert(!r.ok && r.server_error && !r.not_found);
    });
    unavailable->get_message_metadata_async(42, address, 7, 9, [&](webmail::MessageMetadataResult r) {
        ++calls; assert(!r.ok && r.server_error && !r.not_found);
    });
    assert(calls == 6);
}

void pagination_mapping_and_bounded_offset() {
    Fixture f;
    auto db = f.pool->mock_conn();
    int calls = 0;
    const auto page = std::numeric_limits<uint32_t>::max();
    f.repo->list_messages_async(42, address, 7, page, 100, [&](webmail::MessagePageResult result) {
        ++calls;
        assert(result.ok && !result.server_error && !result.not_found);
        assert(result.page == page && result.page_size == 100 && result.total == 15);
        assert(result.mailbox.id == 7 && result.mailbox.box_type == 2);
        assert(result.messages.size() == 1);
        const auto& item = result.messages[0];
        assert(item.id == 9 && item.subject == "subject" && item.send_time == 123);
        assert(item.sender == "from@test" && item.recipients == "a@test,b@test");
        assert(item.status == 0); // Sent always exposes read state
        assert(item.starred && item.important && !item.deleted && item.has_attachment);
    });
    assert((db->last_query_params() == std::vector<std::string>{"7", "42"}));
    db->fire_query(mailbox());
    assert(!calls && f.pool->state->returns == 0);
    assert((db->last_query_params() == std::vector<std::string>{"7", "42"}));
    db->fire_query(rows({{{"total", "15"}}}));
    assert(!calls && f.pool->state->returns == 0);
    assert(db->last_query_sql().find("LIMIT 100 OFFSET 4294967295") != std::string::npos);
    assert((db->last_query_params() == std::vector<std::string>{address, address, address, "7", "42"}));
    db->fire_query(rows({{{"id", "9"}, {"subject", "subject"}, {"sender", "from@test"},
        {"recipients", "a@test,b@test"}, {"send_time", "123"}, {"status", "1"},
        {"is_starred", "1"}, {"is_important", "1"}, {"is_deleted", "0"}, {"has_attachment", "1"}}}));
    assert(calls == 1 && f.pool->state->returns == 1);
}

void pagination_empty_missing_and_failure() {
    for (int failed_stage = 0; failed_stage < 3; ++failed_stage) {
        Fixture f;
        auto db = f.pool->mock_conn();
        int calls = 0;
        f.repo->list_messages_async(42, address, 7, 2, 10, [&](webmail::MessagePageResult result) {
            ++calls;
            assert(!result.ok && !result.server_error && !result.not_found);
            assert(result.page == 2 && result.page_size == 10);
        });
        if (failed_stage > 0) db->fire_query(mailbox());
        if (failed_stage > 1) db->fire_query(rows({{{"total", "1"}}}));
        db->fire_query(nullptr);
        assert(calls == 1 && f.pool->state->returns == 1 && !db->has_pending_query());
    }
    Fixture f;
    auto db = f.pool->mock_conn();
    int calls = 0;
    f.repo->list_messages_async(42, address, 99, 1, 10, [&](webmail::MessagePageResult result) {
        ++calls; assert(!result.ok && result.not_found && !result.server_error);
    });
    assert((db->last_query_params() == std::vector<std::string>{"99", "42"}));
    db->fire_query(rows());
    assert(calls == 1 && !db->has_pending_query());
    f.repo->list_messages_async(42, address, 7, 1, 10, [&](webmail::MessagePageResult result) {
        ++calls; assert(result.ok && result.total == 0 && result.messages.empty());
    });
    db->fire_query(mailbox());
    db->fire_query(rows({{{"total", "0"}}}));
    assert(calls == 2 && f.pool->state->returns == 2 && !db->has_pending_query());
}

void metadata_chain_owns_pool_after_repository_destruction() {
    Fixture f;
    auto db = f.pool->mock_conn();
    auto state = f.pool->state;
    std::weak_ptr<TrackingPool> lifetime = f.pool;
    int calls = 0;
    f.repo->get_message_metadata_async(42, address, 7, 9007199254740993ULL,
        [&](webmail::MessageMetadataResult result) {
            ++calls;
            assert(result.ok && !result.server_error && !result.not_found);
            assert(result.id == 9007199254740993ULL && result.subject == "Hello" && result.send_time == 123);
            assert(result.status == 1 && result.starred && result.important && !result.deleted);
            assert(result.body_key == "objects/opaque-message" && result.sender == "from@test");
            assert((result.recipients == std::vector<std::string>{"a@test", "b@test"}));
            assert(result.attachments.size() == 1 && result.attachments[0].id == 11);
            assert(result.attachments[0].filename == "file.bin" && result.attachments[0].size == 42);
            assert(result.attachments[0].mime_type == "application/octet-stream");
            assert(!lifetime.expired() && state->returns == 0);
        });
    assert((db->last_query_params() == std::vector<std::string>{"9007199254740993", "7", "42"}));
    f.repo.reset(); f.router.reset(); f.pool.reset();
    assert(!lifetime.expired());
    db->fire_query(metadata());
    assert(!calls && state->returns == 0 && !lifetime.expired());
    assert((db->last_query_params() == std::vector<std::string>{"9007199254740993"}));
    db->fire_query(rows({{{"sender", "from@test"}}}));
    assert(!calls && state->returns == 0);
    db->fire_query(rows({{{"recipient", "a@test"}}, {{"recipient", "b@test"}}}));
    assert(!calls && state->returns == 0);
    db->fire_query(rows({{{"id", "11"}, {"filename", "file.bin"}, {"file_size", "42"},
                         {"mime_type", "application/octet-stream"}}}));
    assert(calls == 1 && state->returns == 1 && lifetime.expired() && !db->has_pending_query());
}

void metadata_missing_and_auxiliary_failure() {
    for (bool query_error : {false, true}) {
        Fixture f;
        auto db = f.pool->mock_conn();
        int calls = 0;
        f.repo->get_message_metadata_async(42, address, 7, 9, [&](webmail::MessageMetadataResult result) {
            ++calls; assert(!result.ok && !result.server_error && result.not_found == !query_error);
        });
        db->fire_query(query_error ? nullptr : rows());
        assert(calls == 1 && f.pool->state->returns == 1 && !db->has_pending_query());
    }
    Fixture f;
    auto db = f.pool->mock_conn();
    int calls = 0;
    f.repo->get_message_metadata_async(42, address, 7, 9, [&](webmail::MessageMetadataResult result) {
        ++calls;
        assert(result.ok && result.id == 9007199254740993ULL);
        assert(result.sender.empty() && result.recipients.empty() && result.attachments.empty());
    });
    db->fire_query(metadata());
    for (int auxiliary = 0; auxiliary < 3; ++auxiliary) {
        assert(!calls && db->has_pending_query() && f.pool->state->returns == 0);
        db->fire_query(nullptr);
    }
    assert(calls == 1 && f.pool->state->returns == 1);
}

void inline_driver_completion() {
    Fixture f;
    auto db = f.pool->mock_conn();
    db->set_deferred(false);
    db->push_sync_result(mailbox());
    db->push_sync_result(rows({{{"total", "1"}}}));
    db->push_sync_result(rows());
    int calls = 0;
    f.repo->list_messages_async(42, address, 7, 1, 10, [&](webmail::MessagePageResult result) {
        ++calls; assert(result.ok && result.total == 1 && result.messages.empty());
    });
    assert(calls == 1 && f.pool->state->returns == 1 && !db->has_pending_query());
}
} // namespace

int main() {
    mailbox_mapping_and_availability();
    pagination_mapping_and_bounded_offset();
    pagination_empty_missing_and_failure();
    metadata_chain_owns_pool_after_repository_destruction();
    metadata_missing_and_auxiliary_failure();
    inline_driver_completion();
    std::cout << "Webmail repository contracts passed\n";
}
