#undef NDEBUG
#include "mail_system/back/repository/mailbox_repository.h"
#include "mock_db_pool.h"
#include <cassert>
#include <iostream>

#if defined(BOOST_ASIO_VERSION) || defined(BOOST_VERSION)
#error "Mailbox repository must not expose a network backend"
#endif
namespace {
using namespace mail_system;
using test::MockDbResult;
class TrackingPool : public test::MockDbPool {
public:
    int returns = 0;
    void release_connection(std::shared_ptr<pr::IDBConnection>) override { ++returns; }
};
std::shared_ptr<MockDbResult> rows(std::vector<std::map<std::string, std::string>> value = {}) {
    return std::make_shared<MockDbResult>(std::move(value));
}
void list_and_message_mapping() {
    auto repository = repository::make_sql_mailbox_repository();
    TrackingPool pool;
    auto db = pool.mock_conn();
    int calls = 0;
    repository->get_mailboxes_async(pool.acquire_connection(), 41, [&](bool ok, std::vector<MailboxSummary> boxes) {
        assert(ok && boxes.size() == 1 && boxes[0].id == 7 && boxes[0].name == "收件箱" && boxes[0].box_type == 1);
        ++calls;
    });
    assert(!calls && db->last_query_params() == std::vector<std::string>{"41"});
    db->fire_query(rows({{{"id", "7"}, {"name", "收件箱"}, {"box_type", "1"}}}));
    assert(calls == 1 && pool.returns == 1);
    repository->get_mailbox_mails_async(pool.acquire_connection(), 7, 41,
        [&](bool ok, std::vector<MailboxMailInfo> mails) {
            assert(ok && mails.size() == 1);
            const auto& m = mails[0];
            assert(m.mail_id == 9 && m.sender == "from@test" && m.recipient == "to@test");
            assert(m.subject == "subject" && m.body_path == "objects/9.eml");
            assert(m.is_starred && !m.is_deleted && m.is_important && m.status == 0 && m.send_time == 123);
            ++calls;
        });
    assert((db->last_query_params() == std::vector<std::string>{"7", "41"}));
    db->fire_query(rows({{{"id", "9"}, {"sender", "from@test"}, {"recipient", "to@test"},
                          {"subject", "subject"}, {"body_path", "objects/9.eml"},
                          {"is_starred", "1"}, {"is_deleted", "0"}, {"is_important", "1"}, {"send_time", "123"}}}));
    assert(calls == 2 && pool.returns == 2);
}
void inbox_fallback_keeps_lease() {
    auto repository = repository::make_sql_mailbox_repository();
    TrackingPool pool; auto db = pool.mock_conn();
    auto conn = pool.acquire_connection(); std::weak_ptr<pr::ScopedConnection> lease = conn;
    int calls = 0;
    repository->find_mailbox_id_async(conn, 41, "inbox", [&](uint64_t id) { assert(id == 7); ++calls; });
    conn.reset(); assert(!lease.expired());
    assert((db->last_query_params() == std::vector<std::string>{"41", "inbox"}));
    db->fire_query(rows());
    assert(calls == 0 && db->has_pending_query() && !lease.expired());
    assert(db->last_query_params() == std::vector<std::string>{"41"});
    db->fire_query(rows({{{"id", "7"}}}));
    assert(calls == 1 && lease.expired() && pool.returns == 1);
}
void uidnext_order_and_error_defaults() {
    auto repository = repository::make_sql_mailbox_repository();
    TrackingPool pool; auto db = pool.mock_conn();
    auto conn = pool.acquire_connection(); std::weak_ptr<pr::ScopedConnection> lease = conn;
    int calls = 0;
    repository->get_mailbox_uidnext_async(conn, 7, 41, [&](uint64_t value) { assert(value == 99); ++calls; });
    conn.reset(); assert(!lease.expired() && db->has_pending_execute() && !db->has_pending_query());
    assert((db->last_query_params() == std::vector<std::string>{"7", "7", "7"}));
    // Preserve established semantics: even a failed advance still reads the
    // persisted high-water mark rather than fabricating a smaller UIDNEXT.
    db->fire_execute(false);
    assert(!calls && !lease.expired() && db->has_pending_query());
    assert(db->last_query_params() == std::vector<std::string>{"7"});
    db->fire_query(rows({{{"uidnext", "99"}}}));
    assert(calls == 1 && lease.expired() && pool.returns == 1);
    repository->get_mailbox_count_async(pool.acquire_connection(), 7, 41, [&](size_t count) { assert(count == 5); ++calls; });
    assert((db->last_query_params() == std::vector<std::string>{"7", "41"}));
    db->fire_query(rows({{{"cnt", "5"}}}));
    repository->get_mailbox_unseen_count_async(pool.acquire_connection(), 7, 41, [&](size_t count) { assert(count == 0); ++calls; });
    assert((db->last_query_params() == std::vector<std::string>{"41", "7", "41"}));
    db->fire_query(nullptr);
    repository->get_mailbox_uidnext_async(pr::ScopedConnection::invalid(), 7, 41,
        [&](uint64_t value) { assert(value == 1); ++calls; });
    repository->get_mailboxes_async({}, 41, [&](bool ok, std::vector<MailboxSummary> value) {
        assert(!ok && value.empty()); ++calls;
    });
    assert(calls == 5);
}
} // namespace
int main() {
    list_and_message_mapping(); inbox_fallback_keeps_lease(); uidnext_order_and_error_defaults();
    std::cout << "Mailbox repository contracts passed\n";
}
