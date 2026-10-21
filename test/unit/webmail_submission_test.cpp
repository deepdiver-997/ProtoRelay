#include "test_temp_dir.h"
#include "framework/net/asio_executor.h"
// 2026-10-18: trusted submission owns a committed body until queue admission.
// Exercise provider completion and rejection as consumers; no SMTP session or socket.
// Negative auth callback evidence: docs/bugfixes/2026-10-18-webmail-shared-sessions.md.
#undef NDEBUG
#include "mail_system/back/submission/mail_submission_service.h"
#include "mail_system/back/persist_storage/persistent_queue.h"
#include "mail_system/back/router/static_shard_router.h"
#include "mock_db_pool.h"
#include "fake_account_repository.h"
#include <boost/asio/io_context.hpp>
#include <cassert>
#include <deque>
#include <filesystem>
#include <optional>
#include <unistd.h>

namespace {
using namespace mail_system;
class ManualWorkers final : public pr::ThreadPoolBase {
  public:
    void start() override {}
    void stop(bool = true) override {}
    size_t thread_count() const override { return 1; }
    bool is_running() const override { return true; }
    void drain() {
        size_t count = 0;
        while (!tasks.empty()) {
            assert(++count < 1000);
            auto f = std::move(tasks.front());
            tasks.pop_front();
            f();
        }
    }
    size_t pending() const { return tasks.size(); }

  protected:
    void post_impl(std::function<void()> f) override { tasks.push_back(std::move(f)); }

  private:
    std::deque<std::function<void()>> tasks;
};

class DelayedStorage final : public storage::IStorageProvider {
    class Stream final : public storage::IWriteStream {
      public:
        explicit Stream(DelayedStorage &owner) : owner(owner) {}
        bool write_at(uint64_t, const char *, size_t, storage::IoError &) override {
            owner.exists = true;
            return true;
        }
        bool commit(storage::IoError &) override { return false; }
        void commit_async(CommitCallback cb) override { owner.commit_complete = std::move(cb); }
        void abort() noexcept override { owner.exists = false; }

      private:
        DelayedStorage &owner;
    };

  public:
    std::filesystem::path root =
        test_support::temp_root() / ("webmail-submit-" + std::to_string(getpid()));
    bool exists = false;
    bool removed = false;
    bool fail_open = false;
    storage::CommitCallback commit_complete;
    DelayedStorage() { std::filesystem::create_directories(root); }
    ~DelayedStorage() override { std::filesystem::remove_all(root); }
    bool ensure_ready(storage::IoError &) override { return true; }
    std::string build_mail_body_key(uint64_t id) override { return (root / std::to_string(id)).string(); }
    std::string build_attachment_key(uint64_t, const std::string &) override { return {}; }
    bool append_binary(const std::string &, const char *, size_t, storage::IoError &) override {
        return false;
    }
    bool remove_object(const std::string &key, storage::IoError &) override {
        exists = false;
        removed = true;
        std::filesystem::remove(key);
        return true;
    }
    bool read_all(const std::string &, std::string &, storage::IoError &) override { return false; }
    std::unique_ptr<storage::IWriteStream> open_write(const std::string &, storage::IoError &error) override {
        if (fail_open) {
            error = storage::IoError::permanent("injected storage open failure");
            return nullptr;
        }
        return std::make_unique<Stream>(*this);
    }
    void finish_commit(bool ok) {
        auto cb = std::move(commit_complete);
        assert(cb);
        cb(ok, ok ? storage::IoError{} : storage::IoError::permanent("injected commit failure"));
    }
};

struct Fixture {
    std::shared_ptr<test::MockDbPool> db = std::make_shared<test::MockDbPool>();
    std::shared_ptr<ManualWorkers> workers = std::make_shared<ManualWorkers>();
    std::shared_ptr<DelayedStorage> storage = std::make_shared<DelayedStorage>();
    std::shared_ptr<router::StaticShardRouter> router = std::make_shared<router::StaticShardRouter>(
        std::vector<std::pair<std::string, int>>{}, 0, std::vector<std::shared_ptr<DBPool>>{db},
        std::vector<std::shared_ptr<storage::IStorageProvider>>{storage});
    std::shared_ptr<AccountPolicy> policy =
        std::make_shared<AccountPolicy>(router, std::make_shared<AuthCache>(), workers);
    std::shared_ptr<persist_storage::PersistentQueue> queue =
        std::make_shared<persist_storage::PersistentQueue>(router, workers);
    boost::asio::io_context io;
    std::shared_ptr<submission::MailSubmissionService> service;
    std::optional<submission::Result> result;
    int calls = 0;
    explicit Fixture(std::shared_ptr<repository::IAccountRepository> accounts = nullptr) {
        ServerConfig config;
        config.system_domain = "test.local";
        config.external_delivery_enabled = false;
        config.smtp_daily_send_limit = 0;
        service = std::make_shared<submission::MailSubmissionService>(
            router, workers, policy, queue, [this] { return std::make_shared<pr::net::AsioExecutor>(io.get_executor()); }, config, 3, std::move(accounts));
        auto aliases = std::make_shared<AccountPolicy::AliasCache>(10, std::chrono::seconds(60));
        aliases->put("to@test.local", {});
        policy->set_recipient_caches(std::make_shared<AuthCache>(), aliases);
    }
    void actor(std::string status = "1") {
        db->mock_conn()->push_sync_result(std::make_shared<test::MockDbResult>(
            std::vector<std::map<std::string, std::string>>{{{"id", "42"}, {"status", status}}}));
    }
    void submit(std::string recipient = "to@test.local") {
        submission::Request req;
        req.user_id = 42;
        req.sender = "sender@test.local";
        req.subject = "test";
        req.message_id = "<submission-test@test.local>";
        req.raw = "From: sender@test.local\r\nTo: to@test.local\r\nSubject: test\r\n\r\nhello\r\n";
        req.recipients = {std::move(recipient)};
        service->submit_async(std::move(req), [this](submission::Result r) {
            ++calls;
            result = std::move(r);
        });
        workers->drain();
    }
};
} // namespace

void run_webmail_submission_tests() {
    // The trusted boundary uses the same account repository as authentication,
    // without exposing credential rows or requiring a database in service tests.
    for (auto status : {repository::ActorStatus::denied, repository::ActorStatus::unavailable,
                        repository::ActorStatus::valid}) {
        auto accounts = std::make_shared<test::FakeAccountRepository>();
        accounts->actor_result = status;
        Fixture f(accounts);
        f.storage->fail_open = true;
        f.submit();
        assert(accounts->actor_calls == 1 && accounts->actor_user == 42 && accounts->actor_shard == 3 &&
               accounts->actor_address == "sender@test.local");
        assert(!f.db->mock_conn()->has_pending_query());
        assert(f.calls == 1 && f.result->status == (status == repository::ActorStatus::denied ?
            submission::Status::AuthFailed : submission::Status::Unavailable));
        assert(f.result->stage == (status == repository::ActorStatus::valid ? "storage" : "auth"));
    }
    {
        auto accounts = std::make_shared<test::FakeAccountRepository>(); accounts->route = -1;
        Fixture f(accounts); f.submit();
        assert(!accounts->actor_calls && f.calls == 1 && f.result->status == submission::Status::AuthFailed);
    }
    {
        auto accounts = std::make_shared<test::FakeAccountRepository>();
        std::weak_ptr<test::FakeAccountRepository> weak = accounts;
        Fixture f(accounts);
        submission::Request request;
        request.user_id = 42; request.sender = "sender@test.local"; request.recipients = {"outside@external.test"};
        f.service->submit_async(std::move(request), [&](submission::Result result) {
            assert(result.status == submission::Status::Rejected && !weak.expired()); ++f.calls;
        });
        f.service.reset(); accounts.reset();
        assert(!weak.expired()); f.workers->drain();
        assert(f.calls == 1 && weak.expired());
    }
    {
        Fixture f;
        f.actor("0");
        f.submit();
        assert(f.calls == 1 && f.result->status == submission::Status::AuthFailed && !f.storage->exists);
    }
    {
        Fixture f;
        f.actor();
        f.submit("outside@external.test");
        assert(f.calls == 1 && f.result->status == submission::Status::Rejected && !f.storage->exists);
    }
    {
        Fixture f;
        f.actor();
        f.storage->fail_open = true;
        f.submit();
        assert(f.calls == 1 && f.result->status == submission::Status::Unavailable);
    }
    {
        Fixture f;
        f.actor();
        f.submit();
        assert(f.calls == 0 && f.storage->exists && f.queue->queue_size() == 0);
        f.storage->finish_commit(false);
        f.workers->drain();
        assert(f.calls == 1 && f.result->status == submission::Status::Unavailable && !f.storage->exists);
    }
    {
        Fixture f;
        f.actor();
        f.submit();
        assert(f.calls == 0 && f.storage->exists);
        f.queue->shutdown(); // admission closes while remote commit is in flight
        f.storage->finish_commit(true);
        f.workers->drain();
        assert(f.calls == 1 && f.result->status == submission::Status::Unavailable);
        assert(f.storage->removed && !f.storage->exists);
        assert(std::filesystem::is_empty(f.storage->root)); // includes MIME sidecar
    }
    {
        // A MariaDB callback may run on its IO thread: bcrypt continuation is posted to workers.
        Fixture f;
        AuthCacheEntry cached{"old-password", 1, 42, 0};
        auto cache = std::make_shared<AuthCache>();
        cache->inject("sender@test.local", cached);
        AccountPolicy policy(f.router, cache, f.workers);
        bool called = false;
        policy.login_async(
            "sender@test.local", "old-password",
            [&](AccountAuthResult r) {
                called = true;
                assert(!r.ok);
            },
            true);
        f.workers->drain();
        assert(f.db->mock_conn()->has_pending_query());
        f.db->mock_conn()->fire_query(
            std::make_shared<test::MockDbResult>(std::vector<std::map<std::string, std::string>>{
                {{"id", "42"}, {"status", "1"}, {"password", "different-password"}}}));
        assert(!called && f.workers->pending() == 1);
        f.workers->drain();
        assert(called);
    }
}
