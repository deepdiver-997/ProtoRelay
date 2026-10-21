#undef NDEBUG
#include "mail_system/back/persist_storage/persistent_queue.h"
#if defined(BOOST_VERSION) || defined(MYSQL_VERSION_ID)
#error "PersistentQueue public header must hide backend SDKs and concrete lockfree queues"
#endif
#include "mail_system/back/repository/persistence_repository.h"
#include "mail_system/back/router/static_shard_router.h"
#include "framework/thread_pool/thread_pool_base.h"
#include "mail_system/back/common/logger.h"
#include "mock_db_pool.h"
#include "test_temp_dir.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace mail_system;
using namespace mail_system::persist_storage;
using namespace mail_system::repository;
using namespace std::chrono_literals;
class Workers : public pr::ThreadPoolBase {
public:
    void start() override {}
    void stop(bool = true) override {}
    size_t thread_count() const override { return 1; }
    bool is_running() const override { return true; }
    bool wait_tasks(size_t count = 1) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, 3s, [&] { return tasks.size() >= count; });
    }
    void drain() {
        for (;;) {
            std::function<void()> task;
            { std::lock_guard<std::mutex> lock(mutex); if (tasks.empty()) return; task = std::move(tasks.front()); tasks.pop_front(); }
            task();
        }
    }
    bool throw_post = false;
protected:
    void post_impl(std::function<void()> task) override {
        if (throw_post) throw std::runtime_error("Worker scheduling failed");
        { std::lock_guard<std::mutex> lock(mutex); tasks.push_back(std::move(task)); }
        changed.notify_all();
    }
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::function<void()>> tasks;
};
struct SingleSlotPool final : test::MockDbPool {
    bool active = false;
    int checkouts = 0, returns = 0;
    std::shared_ptr<pr::IDBConnection> get_connection() override {
        assert(!active && "A completion must unwind before its next checkout");
        active = true; ++checkouts; return mock_conn();
    }
    void release_connection(std::shared_ptr<pr::IDBConnection>) override { assert(active); active = false; ++returns; }
};
class FakeRepository final : public IPersistenceRepository {
public:
    PersistenceRequest request;
    PersistenceEraseRequest erased;
    Complete pending;
    std::shared_ptr<SingleSlotPool> pool = std::make_shared<SingleSlotPool>();
    int persists = 0, copies = 0, erases = 0;
    bool throw_persist = false, throw_copies = false, throw_erase = false;
    void persist_async(PersistenceRequest value, Complete complete) override {
        ++persists; request = std::move(value);
        if (throw_persist) throw std::runtime_error("persist failed");
        struct Owner { std::shared_ptr<pr::DBPool> pool; std::shared_ptr<pr::ScopedConnection> lease; };
        auto owner = std::make_shared<Owner>(); owner->pool = pool; owner->lease = pool->acquire_connection();
        pending = [owner, complete = std::move(complete)](PersistenceResult result) mutable { complete(std::move(result)); };
    }
    std::vector<RecipientPersistenceResult> persist_recipient_copies(const PersistenceRequest& value, int shard) override {
        assert(!pool->active); ++copies;
        assert(value.message && shard == 0);
        auto lease = pool->acquire_connection();
        if (throw_copies) throw std::runtime_error("postcommit copy failed");
        return {};
    }
    PersistenceEraseResult erase_metadata(const PersistenceEraseRequest& value) override {
        assert(!pool->active); ++erases; erased = value;
        auto lease = pool->acquire_connection();
        if (throw_erase) throw std::runtime_error("cleanup failed");
        return {};
    }
    void complete(PersistenceDisposition disposition, PersistenceTransaction transaction = PersistenceTransaction::not_started) {
        auto callback = std::move(pending); assert(callback);
        PersistenceResult result; result.disposition = disposition; result.transaction = transaction;
        if (!result.success()) result.error = "primary failed";
        callback(std::move(result));
        // The callback cannot synchronously borrow this still-held single slot.
        assert(pool->active);
    }
};
struct Fixture {
    std::shared_ptr<Workers> workers = std::make_shared<Workers>();
    std::shared_ptr<FakeRepository> repository = std::make_shared<FakeRepository>();
    std::shared_ptr<router::StaticShardRouter> router = std::make_shared<router::StaticShardRouter>(
        std::vector<std::pair<std::string, int>>{}, 0, std::vector<std::shared_ptr<pr::DBPool>>{},
        std::vector<std::shared_ptr<pr::storage::IStorageProvider>>{});
    std::unique_ptr<PersistentQueue> queue = std::make_unique<PersistentQueue>(router, workers, repository);
    static inline uint64_t serial = 1000;
    Fixture() { queue->set_pressure_config({0, 0, 0}); queue->set_local_domain("local.test"); queue->set_max_attempts(17); }
    ~Fixture() { queue->shutdown(); workers->drain(); queue.reset(); }
    std::unique_ptr<::mail> message(bool body = true) {
        auto value = std::make_unique<::mail>(); value->id = ++serial; value->from = "sender@local.test";
        value->to = {"recipient@remote.test"}; value->ids = {value->id + 10000}; value->send_time = 10;
        if (body) {
            value->body_path = test_support::temp_path("queue-contract-" + std::to_string(value->id));
            std::ofstream out(value->body_path); out << "body";
        }
        return value;
    }
    SubmitOwnedMailResult submit() {
        auto result = queue->submit_owned_mail(message()); assert(result.accepted);
        assert(workers->wait_tasks()); workers->drain(); assert(repository->pending); return result;
    }
    void finalize() { assert(workers->wait_tasks()); workers->drain(); }
};
void committed_result_survives_cancel_shutdown_and_postcommit_exception() {
    for (bool shutdown : {false, true}) {
        Fixture f; auto submission = f.submit(); const auto body = f.repository->request.message->body_path;
        f.repository->throw_copies = true;
        if (shutdown) f.queue->shutdown(); else submission.ticket.request_cancel();
        f.repository->complete(PersistenceDisposition::committed, PersistenceTransaction::committed);
        assert(!submission.ticket.completion->result() && f.repository->copies == 0);
        f.finalize(); auto result = submission.ticket.completion->result();
        assert(result && result->status == PersistStatus::SUCCESS && !result->deduplicated);
        assert(f.repository->copies == 1 && f.repository->erases == 0 && std::filesystem::exists(body));
        assert(!f.queue->inflight_count() && f.repository->pool->returns == 2);
        std::filesystem::remove(body);
    }
}
void cleanup_is_deferred_and_keeps_distinct_terminal_outcomes() {
    for (auto disposition : {PersistenceDisposition::failed, PersistenceDisposition::deduplicated, PersistenceDisposition::file_only}) {
        Fixture f; auto submission = f.submit(); const auto body = f.repository->request.message->body_path;
        f.repository->throw_erase = true;
        f.repository->complete(disposition);
        assert(f.repository->erases == 0 && std::filesystem::exists(body));
        f.finalize(); const auto result = submission.ticket.completion->result(); assert(result);
        const bool failed = disposition == PersistenceDisposition::failed;
        assert(result->status == (failed ? PersistStatus::FAILED : PersistStatus::SUCCESS));
        assert(result->deduplicated == (disposition == PersistenceDisposition::deduplicated));
        assert(f.repository->copies == 0 && f.repository->erases == (failed ? 1 : 0));
        assert(std::filesystem::exists(body) == (disposition == PersistenceDisposition::file_only));
        assert(!f.queue->inflight_count()); std::filesystem::remove(body);
    }
}
void snapshots_and_repository_replacement_do_not_change_inflight_work() {
    Fixture f; auto message = f.message(); const auto body = message->body_path;
    auto submission = f.queue->submit_owned_mail(std::move(message)); assert(submission.accepted && f.workers->wait_tasks());
    // The queue worker captured immutable options/repository before publishing
    // this Worker job. Later setters affect subsequent work only.
    auto replacement = std::make_shared<FakeRepository>();
    f.queue->set_local_domain("changed.test"); f.queue->set_max_attempts(99); f.queue->set_persistence_repository(replacement);
    f.workers->drain();
    assert(f.repository->request.options.local_domain == "local.test" && f.repository->request.options.max_attempts == 17);
    assert(!replacement->persists);
    f.repository->complete(PersistenceDisposition::committed, PersistenceTransaction::committed); f.finalize();
    assert(f.repository->copies == 1 && !replacement->copies && submission.ticket.status == PersistStatus::SUCCESS);
    std::filesystem::remove(body);
    bool rejected = false;
    try { f.queue->set_persistence_repository({}); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
}
void cancellation_pressure_and_scheduling_exceptions_finish_once() {
    {
        Fixture f; f.queue->set_pressure_config({1, 0, 0});
        auto first_mail = f.message(); const auto body = first_mail->body_path;
        auto submission = f.queue->submit_owned_mail(std::move(first_mail)); assert(submission.accepted && f.workers->wait_tasks());
        auto second = f.queue->submit_owned_mail(f.message(false)); assert(!second.accepted && second.rejected_mail);
        submission.ticket.request_cancel(); f.workers->drain();
        assert(submission.ticket.status == PersistStatus::CANCELLED && !f.repository->persists && !std::filesystem::exists(body));
        assert(!f.queue->inflight_count());
    }
    for (bool fail_after_primary : {false, true}) {
        Fixture f;
        if (fail_after_primary) {
            auto submission = f.submit(); const auto body = f.repository->request.message->body_path;
            f.workers->throw_post = true;
            f.repository->complete(PersistenceDisposition::committed, PersistenceTransaction::committed);
            assert(submission.ticket.status == PersistStatus::SUCCESS && std::filesystem::exists(body) && !f.queue->inflight_count());
            assert(!f.repository->erases); std::filesystem::remove(body);
        } else {
            f.repository->throw_persist = true;
            auto value = f.message(); const auto body = value->body_path;
            auto submission = f.queue->submit_owned_mail(std::move(value)); assert(submission.accepted && f.workers->wait_tasks());
            f.workers->drain();
            assert(submission.ticket.status == PersistStatus::FAILED && !std::filesystem::exists(body) && !f.queue->inflight_count());
        }
    }
}
class BlockingAdmissionWorkers final : public Workers {
public:
    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    bool entered = false, released = false;
    bool wait_entered() { std::unique_lock<std::mutex> lock(gate_mutex); return gate_changed.wait_for(lock, 3s, [&] { return entered; }); }
    void release() { { std::lock_guard<std::mutex> lock(gate_mutex); released = true; } gate_changed.notify_all(); }
protected:
    void post_impl(std::function<void()> task) override {
        { std::unique_lock<std::mutex> lock(gate_mutex); entered = true; gate_changed.notify_all(); gate_changed.wait(lock, [&] { return released; }); }
        Workers::post_impl(std::move(task));
    }
};
void fixed_capacity_admission_fails_without_waiting_for_consumer() {
    auto workers = std::make_shared<BlockingAdmissionWorkers>();
    auto repository = std::make_shared<FakeRepository>();
    auto router = std::make_shared<router::StaticShardRouter>(std::vector<std::pair<std::string, int>>{}, 0,
        std::vector<std::shared_ptr<pr::DBPool>>{}, std::vector<std::shared_ptr<pr::storage::IStorageProvider>>{});
    auto queue = std::make_unique<PersistentQueue>(router, workers, repository); queue->set_pressure_config({0, 0, 0});
    auto make = [](uint64_t id) { auto value = std::make_unique<::mail>(); value->id = id; return value; };
    auto first = queue->submit_owned_mail(make(1)); assert(first.accepted && workers->wait_entered());
    for (uint64_t id = 2; id < 16386; ++id) assert(queue->submit_owned_mail(make(id)).accepted);
    assert(queue->queue_size() == 16384 && queue->inflight_count() == 16385);
    const auto started = std::chrono::steady_clock::now(); auto rejected = queue->submit_owned_mail(make(20000));
    assert(!rejected.accepted && rejected.rejected_mail && rejected.error == "persistent queue is full");
    assert(std::chrono::steady_clock::now() - started < 1s && queue->queue_size() == 16384);
    queue->shutdown(); workers->release(); assert(workers->wait_tasks()); workers->drain(); queue.reset();
    assert(first.ticket.status == PersistStatus::CANCELLED && !repository->persists);
}
} // namespace
int main() {
    Logger::get_instance().set_handler({});
    committed_result_survives_cancel_shutdown_and_postcommit_exception(); cleanup_is_deferred_and_keeps_distinct_terminal_outcomes();
    snapshots_and_repository_replacement_do_not_change_inflight_work(); cancellation_pressure_and_scheduling_exceptions_finish_once();
    fixed_capacity_admission_fails_without_waiting_for_consumer();
    std::cout << "PersistentQueue contracts passed\n";
}
