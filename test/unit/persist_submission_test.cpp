// 持久化完成边界：订阅竞态、事务提交、取消/超时、关机与背压。
// 2026-10-18 前科及负验证：docs/bugfixes/2026-10-18-persist-completion-callback.md。
#undef NDEBUG
#include <cassert>
#include "mail_system/back/persist_storage/persistent_queue.h"
#include "framework/thread_pool/worker_thread_pool.h"
#include "mail_system/back/router/static_shard_router.h"
#include "mail_system/back/storage/local_file_storage_provider.h"
#include "mock_db_pool.h"
#include <boost/asio.hpp>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <unistd.h>
using namespace mail_system;
using namespace mail_system::persist_storage;
namespace asio = boost::asio;
using namespace std::chrono_literals;

class ControlledConnection : public test::MockDbConnection {
public:
    ControlledConnection() { set_deferred(false); }
    std::atomic<bool> fail_metadata{false}, committed{false};
    std::atomic<bool> sent_missing{false}, sent_insert_failure{false}, sent_inserted{false};
    std::atomic<int> deletes{0}, rollbacks{0};
    bool execute(const std::string& sql) override {
        if (sql.rfind("DELETE", 0) == 0) ++deletes;
        return true;
    }
    void async_execute(const std::string& sql, pr::ExecuteCallback cb) override {
        cb(!fail_metadata && execute(sql));
    }
    void async_query(const std::string& sql, const std::vector<std::string>& params, pr::QueryCallback cb) override {
        if (sql.find("mb.box_type=2") != std::string::npos) {
            assert(params == std::vector<std::string>{"sender@test.local"});
            cb(std::make_shared<test::MockDbResult>(sent_missing ? std::vector<std::map<std::string,std::string>>{} :
                std::vector<std::map<std::string,std::string>>{{{"mailbox_id","7"},{"user_id","9"}}}));
        } else test::MockDbConnection::async_query(sql, params, std::move(cb));
    }
    void async_execute(const std::string& sql, const std::vector<std::string>& params, pr::ExecuteCallback cb) override {
        assert(sql.find("INSERT INTO mail_mailbox") == 0);
        assert(params.size() == 3 && params[1] == "7" && params[2] == "9" && !committed);
        sent_inserted = !sent_insert_failure; cb(!sent_insert_failure);
    }
    void async_commit(pr::ExecuteCallback cb) override {
        std::lock_guard<std::mutex> lock(mutex_); commit_ = std::move(cb); ready_.notify_all();
    }
    void async_rollback(pr::ExecuteCallback cb) override {
        std::lock_guard<std::mutex> lock(mutex_); rollback_ = std::move(cb); ready_.notify_all();
    }
    bool wait_commit() {
        std::unique_lock<std::mutex> lock(mutex_);
        return ready_.wait_for(lock, 2s, [&] { return bool(commit_); });
    }
    bool wait_rollback() {
        std::unique_lock<std::mutex> lock(mutex_);
        return ready_.wait_for(lock, 2s, [&] { return bool(rollback_); });
    }
    void complete_commit(bool ok = true) {
        pr::ExecuteCallback cb;
        { std::lock_guard<std::mutex> lock(mutex_); cb = std::move(commit_); }
        assert(cb); committed = ok; cb(ok);
    }
    void complete_rollback() {
        pr::ExecuteCallback cb;
        { std::lock_guard<std::mutex> lock(mutex_); cb = std::move(rollback_); }
        assert(cb); ++rollbacks; cb(true);
    }
private:
    std::mutex mutex_;
    std::condition_variable ready_;
    pr::ExecuteCallback commit_, rollback_;
};
class ControlledPool : public pr::DBPool {
public:
    std::shared_ptr<ControlledConnection> connection = std::make_shared<ControlledConnection>();
    size_t get_pool_size() const override { return 1; }
    size_t get_available_connections() const override { return 1; }
    size_t get_max_pool_size() const override { return 1; }
    size_t get_active_connections() const override { return 0; }
    void close() override {}
    std::shared_ptr<pr::IDBConnection> get_connection() override { return connection; }
    void release_connection(std::shared_ptr<pr::IDBConnection>) override {}
protected:
    void initialize_pool() override {}
    std::shared_ptr<pr::IDBConnection> create_connection() override { return connection; }
};
struct Fixture {
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> guard{io.get_executor()};
    std::thread thread{[this] { io.run(); }};
    std::shared_ptr<pr::WorkerThreadPool> workers = std::make_shared<pr::WorkerThreadPool>(1);
    std::shared_ptr<ControlledPool> db;
    std::shared_ptr<router::StaticShardRouter> router;
    std::shared_ptr<PersistentQueue> queue;
    std::filesystem::path directory;
    static std::atomic<int> serial;
    Fixture(bool database = true) {
        workers->start();
        directory = std::filesystem::temp_directory_path() /
            ("persist-completion-" + std::to_string(getpid()) + "-" + std::to_string(++serial));
        auto provider = std::make_shared<storage::LocalFileStorageProvider>((directory / "mail").string(), (directory / "att").string());
        storage::IoError storage_error; assert(provider->ensure_ready(storage_error));
        if (database) db = std::make_shared<ControlledPool>();
        router = std::make_shared<router::StaticShardRouter>(std::vector<std::pair<std::string,int>>{}, 0,
            db ? std::vector<std::shared_ptr<DBPool>>{db} : std::vector<std::shared_ptr<DBPool>>{},
            std::vector<std::shared_ptr<storage::IStorageProvider>>{provider});
        queue = std::make_shared<PersistentQueue>(router, workers);
        queue->set_pressure_config({8, 0, 0}); queue->set_local_domain("test.local");
    }
    ~Fixture() {
        queue->shutdown(); workers->stop(); queue.reset();
        guard.reset(); thread.join();
        std::filesystem::remove_all(directory);
    }
    std::unique_ptr<::mail> message(uint64_t id = 100) {
        auto mail = std::make_unique<::mail>();
        mail->id = id; mail->from = "sender@test.local"; mail->to = {"rcpt@test.local"};
        mail->ids = {id + 1000}; mail->subject = "test"; mail->send_time = std::time(nullptr);
        mail->body_path = router->get_storage(0)->build_mail_body_key(id);
        std::ofstream file(mail->body_path); assert(file); file << "Subject: test\r\n\r\nbody\r\n"; file.close();
        return mail;
    }
};
std::atomic<int> Fixture::serial{0};

void test_committed_cancel() {
    Fixture f;
    auto mail = f.message(); const auto body = mail->body_path;
    auto submission = f.queue->submit_owned_mail(std::move(mail)); assert(submission.accepted);
    assert(f.db->connection->wait_commit());
    std::promise<PersistResult> reply; auto result = reply.get_future();
    submission.ticket.async_wait(f.io.get_executor(), 2s, [&](PersistResult r) { reply.set_value(r); });
    assert(result.wait_for(20ms) == std::future_status::timeout);
    submission.ticket.request_cancel(); f.db->connection->complete_commit();
    assert(result.wait_for(2s) == std::future_status::ready);
    assert(result.get().status == PersistStatus::SUCCESS && submission.ticket.status == PersistStatus::SUCCESS);
    assert(f.db->connection->committed && f.db->connection->deletes == 0 && std::filesystem::exists(body));
    // 完成之后再订阅仍能收到同一终态，且不能二次覆盖。
    std::promise<PersistResult> later; auto late = later.get_future();
    submission.ticket.async_wait(f.io.get_executor(), 2s, [&](PersistResult r) { later.set_value(r); });
    assert(late.wait_for(2s) == std::future_status::ready && late.get().status == PersistStatus::SUCCESS);
    assert(!submission.ticket.complete(PersistStatus::FAILED, "duplicate callback"));
}
void test_timeout_late_commit() {
    Fixture f;
    auto mail = f.message(); const auto body = mail->body_path;
    auto submission = f.queue->submit_owned_mail(std::move(mail)); assert(submission.accepted);
    assert(f.db->connection->wait_commit());
    std::promise<PersistResult> reply; auto result = reply.get_future(); std::atomic<int> calls{0};
    submission.ticket.async_wait(f.io.get_executor(), 30ms, [&](PersistResult r) { ++calls; reply.set_value(r); });
    assert(result.wait_for(2s) == std::future_status::ready);
    assert(result.get().timed_out && !submission.ticket.is_cancel_requested());
    submission.ticket.request_cancel();
    assert(!submission.ticket.completion->result());
    f.db->connection->complete_commit();
    assert(submission.ticket.completion->result()->status == PersistStatus::SUCCESS);
    assert(calls == 1 && f.db->connection->deletes == 0 && std::filesystem::exists(body));
}
void test_failed_rollback(bool commit_failure) {
    Fixture f;
    f.db->connection->fail_metadata = !commit_failure;
    auto mail = f.message(); const auto body = mail->body_path;
    auto submission = f.queue->submit_owned_mail(std::move(mail)); assert(submission.accepted);
    std::promise<PersistResult> reply; auto result = reply.get_future();
    submission.ticket.async_wait(f.io.get_executor(), 2s, [&](PersistResult r) { reply.set_value(r); });
    if (commit_failure) { assert(f.db->connection->wait_commit()); f.db->connection->complete_commit(false); }
    assert(f.db->connection->wait_rollback());
    assert(result.wait_for(20ms) == std::future_status::timeout); // 回滚完成前不能释放事务/发布失败
    f.db->connection->complete_rollback();
    assert(result.wait_for(2s) == std::future_status::ready && result.get().status == PersistStatus::FAILED);
    assert(f.db->connection->rollbacks == 1 && !std::filesystem::exists(body));
}
void test_cancel_shutdown_pressure(bool shutdown) {
    Fixture f(false); f.queue->set_pressure_config({1, 0, 0});
    std::promise<void> entered, release; auto gate = release.get_future().share();
    f.workers->post([&] { entered.set_value(); gate.wait(); }); entered.get_future().wait();
    auto mail = f.message(); const auto body = mail->body_path;
    auto submission = f.queue->submit_owned_mail(std::move(mail)); assert(submission.accepted);
    auto rejected = f.queue->submit_owned_mail(f.message(101));
    assert(!rejected.accepted && rejected.rejected_mail && rejected.rejected_mail->id == 101);
    std::promise<PersistResult> reply; auto result = reply.get_future();
    submission.ticket.async_wait(f.io.get_executor(), 2s, [&](PersistResult r) { reply.set_value(r); });
    if (shutdown) f.queue->shutdown(); else submission.ticket.request_cancel();
    release.set_value();
    assert(result.wait_for(2s) == std::future_status::ready && result.get().status == PersistStatus::CANCELLED);
    assert(!std::filesystem::exists(body));
}
void test_observer_race() {
    asio::io_context io; auto guard = asio::make_work_guard(io); std::thread runner([&] { io.run(); });
    for (int i = 1; i <= 100; ++i) {
        PersistSubmissionTicket ticket; ticket.mail_id = i;
        assert(!ticket.complete(PersistStatus::PROCESSING));
        std::promise<PersistResult> reply; auto result = reply.get_future();
        std::thread publisher([&] { assert(ticket.complete(PersistStatus::SUCCESS)); });
        ticket.async_wait(io.get_executor(), 1s, [&](PersistResult r) { reply.set_value(r); });
        publisher.join(); assert(result.wait_for(1s) == std::future_status::ready && result.get().status == PersistStatus::SUCCESS);
    }
    guard.reset(); runner.join();
}
void test_independent_waiters() {
    asio::io_context io; auto guard = asio::make_work_guard(io); std::thread runner([&] { io.run(); });
    PersistSubmissionTicket ticket; ticket.mail_id = 88;
    std::promise<PersistResult> first, second;
    auto short_wait = first.get_future(); auto long_wait = second.get_future();
    ticket.async_wait(io.get_executor(), 10ms, [&](PersistResult r) { first.set_value(r); });
    ticket.async_wait(io.get_executor(), 1s, [&](PersistResult r) { second.set_value(r); });
    assert(short_wait.wait_for(1s) == std::future_status::ready && short_wait.get().timed_out);
    assert(!ticket.is_cancel_requested() && !ticket.completion->result());
    assert(ticket.complete(PersistStatus::SUCCESS));
    assert(long_wait.wait_for(1s) == std::future_status::ready && long_wait.get().status == PersistStatus::SUCCESS);
    guard.reset(); runner.join();
}
void test_atomic_sent_copy(int failure) {
    Fixture f;
    f.db->connection->sent_missing = failure == 1;
    f.db->connection->sent_insert_failure = failure == 2;
    auto mail = f.message(); mail->save_sent_copy = true; const auto body = mail->body_path;
    auto submission = f.queue->submit_owned_mail(std::move(mail)); assert(submission.accepted);
    std::promise<PersistResult> reply; auto result = reply.get_future();
    submission.ticket.async_wait(f.io.get_executor(), 2s, [&](PersistResult r) { reply.set_value(r); });
    if (!failure) {
        assert(f.db->connection->wait_commit() && f.db->connection->sent_inserted);
        assert(result.wait_for(20ms) == std::future_status::timeout);
        f.db->connection->complete_commit();
        assert(result.wait_for(2s) == std::future_status::ready && result.get().status == PersistStatus::SUCCESS);
        assert(std::filesystem::exists(body));
    } else {
        assert(f.db->connection->wait_rollback() && !f.db->connection->committed);
        f.db->connection->complete_rollback();
        assert(result.wait_for(2s) == std::future_status::ready && result.get().status == PersistStatus::FAILED);
        assert(!std::filesystem::exists(body));
    }
}
int main() {
    test_atomic_sent_copy(0); test_atomic_sent_copy(1); test_atomic_sent_copy(2);
    test_observer_race(); test_independent_waiters(); test_committed_cancel(); test_timeout_late_commit();
    test_failed_rollback(false); test_failed_rollback(true);
    test_cancel_shutdown_pressure(false); test_cancel_shutdown_pressure(true);
    std::cout << "PASS: persistence completion races, commit/rollback boundary, late cancellation, timeout, shutdown and pressure\n";
}
