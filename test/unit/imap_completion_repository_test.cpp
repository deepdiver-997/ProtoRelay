#undef NDEBUG
#include "mail_system/back/repository/imap_message_repository.h"
#include "mail_system/back/repository/imap_folder_repository.h"
#include "mail_system/back/common/imap_append_service.h"
#include "mock_db_pool.h"
#include <cassert>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(BOOST_ASIO_VERSION) || defined(BOOST_VERSION)
#error "IMAP application contracts must not expose an Asio backend"
#endif
namespace {
using namespace mail_system;
using namespace mail_system::repository;
struct TrackingPool final : test::MockDbPool {
    int returns = 0;
    void release_connection(std::shared_ptr<pr::IDBConnection>) override { ++returns; }
};
std::shared_ptr<test::MockDbResult> rows(std::vector<std::map<std::string, std::string>> values = {}) {
    return std::make_shared<test::MockDbResult>(std::move(values));
}
class ManualWorkers final : public pr::ThreadPoolBase {
public:
    std::deque<std::function<void()>> tasks;
    bool running = false;
    void start() override {}
    void stop(bool = true) override {}
    size_t thread_count() const override { return 1; }
    bool is_running() const override { return true; }
    void drain() {
        while (!tasks.empty()) {
            auto task = std::move(tasks.front()); tasks.pop_front();
            running = true; task(); running = false;
        }
    }
protected:
    void post_impl(std::function<void()> task) override { tasks.push_back(std::move(task)); }
};
class RecordingStorage final : public pr::storage::IStorageProvider {
public:
    std::shared_ptr<ManualWorkers> worker;
    std::map<std::string, std::string> objects;
    bool fail_append = false, fail_remove = false;
    int appends = 0, removes = 0;
    explicit RecordingStorage(std::shared_ptr<ManualWorkers> w) : worker(std::move(w)) {}
    bool ensure_ready(pr::IoError&) override { return true; }
    std::string build_mail_body_key(uint64_t id) override { return "opaque/body/" + std::to_string(id); }
    std::string build_attachment_key(uint64_t id, const std::string& name) override { return std::to_string(id) + '/' + name; }
    bool append_binary(const std::string& key, const char* data, size_t size, pr::IoError& error) override {
        assert(worker->running); ++appends; objects[key].append(data, size);
        if (fail_append) { error = pr::IoError::permanent("partial body write"); return false; }
        return true;
    }
    bool remove_object(const std::string& key, pr::IoError& error) override {
        assert(worker->running); ++removes;
        if (fail_remove) { error = pr::IoError::permanent("remove failed"); return false; }
        objects.erase(key); return true;
    }
    bool read_all(const std::string& key, std::string& bytes, pr::IoError&) override {
        auto it = objects.find(key); if (it == objects.end()) return false;
        bytes = it->second; return true;
    }
};
ImapAppendRequest append_request() { return {41, 7, "probe subject", "Subject: probe subject\r\n\r\nbody", "fallback@test", 1}; }
void metadata_read_mapping_and_lease() {
    auto repository = make_sql_imap_message_repository();
    TrackingPool pool; auto db = pool.mock_conn(); int calls = 0;
    repository->get_mail_async(pool.acquire_connection(), 81, [&](bool ok, MailboxMailInfo value) {
        assert(ok && value.mail_id == 81 && value.subject == "subject" && value.body_path == "opaque/81" && value.send_time == 10);
        assert(!pool.returns); ++calls;
    });
    assert((db->last_query_params() == std::vector<std::string>{"81"}));
    db->fire_query(rows({{{"id", "81"}, {"subject", "subject"}, {"body_path", "opaque/81"}, {"send_time", "10"}}}));
    assert(calls == 1 && pool.returns == 1);
    repository->get_sender_async(pool.acquire_connection(), 81, [&](std::string value) { assert(value == "sender@test"); ++calls; });
    db->fire_query(rows({{{"sender", "sender@test"}}}));
    repository->get_recipients_async(pool.acquire_connection(), 81, [&](std::vector<std::string> value) {
        assert((value == std::vector<std::string>{"one@test", "two@test"})); ++calls;
    });
    db->fire_query(rows({{{"recipient", "one@test"}}, {{"recipient", "two@test"}}}));
    repository->get_user_address_async(pool.acquire_connection(), 41, [&](std::string value) { assert(value.empty()); ++calls; });
    db->fire_query(nullptr);
    repository->get_mail_async({}, 81, [&](bool ok, MailboxMailInfo) { assert(!ok); ++calls; });
    assert(calls == 5 && pool.returns == 4);
}
void folders_fail_closed_and_keep_scope() {
    auto repository = make_sql_imap_folder_repository();
    TrackingPool pool; auto db = pool.mock_conn(); int calls = 0;
    repository->create_async(pool.acquire_connection(), 41, "项目", [&](ImapFolderResult value) { assert(value.status == ImapFolderStatus::ok); ++calls; });
    assert((db->last_query_params() == std::vector<std::string>{"41", "项目"}));
    db->fire_execute(true);
    repository->rename_async(pool.acquire_connection(), {41, 7}, "renamed", [&](ImapFolderResult value) { assert(value.status == ImapFolderStatus::database_error); ++calls; });
    assert((db->last_query_params() == std::vector<std::string>{"renamed", "7", "41"}));
    assert(db->last_query_sql().find("AND user_id = ?") != std::string::npos);
    db->fire_execute(false);
    for (const auto& result : std::vector<std::shared_ptr<pr::IDBResult>>{nullptr, rows(), rows({{{"is_system", "1"}}})}) {
        repository->remove_async(pool.acquire_connection(), {41, 7}, [&](ImapFolderResult value) {
            assert(value.status != ImapFolderStatus::ok && !value.may_have_changed); ++calls;
        });
        assert((db->last_query_params() == std::vector<std::string>{"7", "41"}));
        db->fire_query(result);
        assert(!db->has_pending_execute());
    }
    repository->remove_async(pool.acquire_connection(), {41, 7}, [&](ImapFolderResult value) {
        assert(value.status == ImapFolderStatus::database_error && value.may_have_changed); ++calls;
    });
    db->fire_query(rows({{{"is_system", "0"}}}));
    assert(db->has_pending_execute() && db->last_query_sql().find("DELETE FROM mail_mailbox") == 0);
    db->fire_execute(false);
    assert(!db->has_pending_execute()); // Never delete the folder after failed unlinking.
    repository->remove_async(pool.acquire_connection(), {41, 7}, [&](ImapFolderResult value) {
        assert(value.status == ImapFolderStatus::ok && value.may_have_changed); ++calls;
    });
    db->fire_query(rows({{{"is_system", "0"}}}));
    db->fire_execute(true);
    assert(db->last_query_sql().find("DELETE FROM mailboxes") == 0);
    assert((db->last_query_params() == std::vector<std::string>{"7", "41"}));
    db->fire_execute(true);
    assert(calls == 7 && pool.returns == 7);
}
void append_success_and_stable_body_key() {
    auto worker = std::make_shared<ManualWorkers>();
    auto storage = std::make_shared<RecordingStorage>(worker);
    auto service = make_imap_append_service(worker);
    TrackingPool pool; auto db = pool.mock_conn(); int calls = 0;
    ImapAppendResult output;
    service->append_async(pool.acquire_connection(), storage, append_request(), [&](ImapAppendResult value) {
        assert(!worker->running && pool.returns == 0); output = std::move(value); ++calls;
    });
    assert(storage->objects.empty() && !db->has_pending_execute());
    worker->drain();
    const auto params = db->last_query_params();
    assert(params.size() == 4 && params[1] == "probe subject" && !params[2].empty());
    assert(params[2] == storage->build_mail_body_key(std::stoull(params[0])));
    assert(storage->objects.at(params[2]) == append_request().body);
    db->fire_execute(true);
    assert((db->last_query_params() == std::vector<std::string>{"41"}));
    db->fire_query(rows({{{"mail_address", "owner@test"}}}));
    const auto recipient = db->last_query_params();
    assert(recipient.size() == 5 && recipient[1] == params[0] && recipient[2] == "owner@test" && recipient[3] == "owner@test" && recipient[4] == "1");
    db->fire_execute(true);
    assert((db->last_query_params() == std::vector<std::string>{params[0], "7", "41"}));
    db->fire_execute(true);
    assert(calls == 1 && output.ok && output.mail_id == std::stoull(params[0]) && output.body_key == params[2]);
    assert(pool.returns == 1 && storage->removes == 0);
}
void append_metadata_failure_cleans_on_worker() {
    auto worker = std::make_shared<ManualWorkers>(); auto storage = std::make_shared<RecordingStorage>(worker);
    auto service = make_imap_append_service(worker);
    TrackingPool pool; auto db = pool.mock_conn(); int calls = 0;
    service->append_async(pool.acquire_connection(), storage, append_request(), [&](ImapAppendResult result) {
        assert(!result.ok && result.cleanup_complete && result.error == "Insert mail record failed"); ++calls;
    });
    worker->drain(); db->fire_execute(false);
    assert(!calls && !pool.returns && !storage->objects.empty() && storage->removes == 0);
    worker->drain();
    assert(calls == 1 && pool.returns == 1 && storage->objects.empty() && storage->removes == 1);
}
void append_link_failure_attempts_every_compensation() {
    for (bool fail_recipient : {true, false}) {
        auto worker = std::make_shared<ManualWorkers>(); auto storage = std::make_shared<RecordingStorage>(worker);
        auto service = make_imap_append_service(worker);
        TrackingPool pool; auto db = pool.mock_conn(); int calls = 0;
        service->append_async(pool.acquire_connection(), storage, append_request(), [&](ImapAppendResult result) {
            assert(!result.ok && !result.cleanup_complete && result.mail_id == 0); ++calls;
        });
        worker->drain(); db->fire_execute(true); db->fire_query(nullptr);
        assert(db->last_query_params()[2] == "fallback@test" && db->last_query_params()[3] == "fallback@test");
        db->fire_execute(!fail_recipient);
        if (!fail_recipient) db->fire_execute(false);
        assert(db->last_query_sql().find("DELETE FROM mail_mailbox") == 0);
        db->fire_execute(false);
        assert(db->last_query_sql().find("DELETE FROM mail_recipients") == 0);
        db->fire_execute(true);
        assert(db->last_query_sql().find("DELETE FROM mails") == 0);
        db->fire_execute(false);
        assert(!calls && !storage->objects.empty() && !pool.returns);
        worker->drain();
        assert(calls == 1 && storage->objects.empty() && pool.returns == 1);
    }
}
void append_partial_write_and_cleanup_failure() {
    for (bool fail_remove : {true, false}) {
        auto worker = std::make_shared<ManualWorkers>(); auto storage = std::make_shared<RecordingStorage>(worker);
        storage->fail_append = true; storage->fail_remove = fail_remove;
        auto service = make_imap_append_service(worker);
        TrackingPool pool; int calls = 0;
        service->append_async(pool.acquire_connection(), storage, append_request(), [&](ImapAppendResult result) {
            assert(!result.ok && result.cleanup_complete == !fail_remove); ++calls;
        });
        worker->drain();
        assert(calls == 1 && storage->removes == 1 && storage->objects.empty() == !fail_remove);
        assert(!pool.mock_conn()->has_pending_execute() && pool.returns == 1);
    }
}
} // namespace
int main() {
    metadata_read_mapping_and_lease(); folders_fail_closed_and_keep_scope();
    append_success_and_stable_body_key(); append_metadata_failure_cleans_on_worker();
    append_link_failure_attempts_every_compensation(); append_partial_write_and_cleanup_failure();
    std::cout << "IMAP metadata/folder/APPEND contracts passed\n";
}
