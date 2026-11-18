#ifdef NDEBUG
#undef NDEBUG
#endif
#include "test_temp_dir.h"
#include "mail_system/back/mailServer/smtps_server.h"
#include "mail_system/back/mailServer/fsm/smtps/traditional_smtps_fsm.tpp"
#include "mail_system/back/mailServer/session/smtps_session.tpp"
#include "framework/session_base.tpp"
#include "framework/storage/local_file_write_stream.h"
#include "framework/storage/local_storage_runtime.h"
#include "framework/thread_pool/io_thread_pool.h"
#include "framework/thread_pool/worker_thread_pool.h"
#include "mail_system/back/router/static_shard_router.h"
#include "mail_system/back/storage/local_file_storage_provider.h"
#include "mock_connection.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;
using namespace mail_system;

namespace {
enum class Scenario { Success, Closed, Reset, Failure };

struct Gate {
    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> ready = release.get_future().share();
    bool fail = false;
    int sync(int fd) {
        entered.set_value();
        assert(ready.wait_for(5s) == std::future_status::ready);
        if (fail) { errno = ENOSPC; return -1; }
        return ::fsync(fd);
    }
};

struct GatedStorage : storage::LocalFileStorageProvider {
    GatedStorage(const std::filesystem::path& root, std::shared_ptr<pr::LocalStorageRuntime> runtime, Gate& gate)
        : LocalFileStorageProvider((root / "mail").string(), (root / "att").string(), std::move(runtime)), gate(gate) {}
    std::unique_ptr<storage::IWriteStream> open_write(const std::string& key, storage::IoError& error) override {
        pr::LocalFileWriteStream::Operations ops;
        ops.fsync = [this](int fd) { return gate.sync(fd); };
        return pr::LocalFileWriteStream::open(key, error, local_storage_runtime(), std::move(ops));
    }
    std::unique_ptr<storage::IReadStream> open_read(const std::string& key, storage::IoError& error) override {
        {
            std::lock_guard<std::mutex> lock(mutex);
            reader = std::this_thread::get_id();
        }
        return LocalFileStorageProvider::open_read(key, error);
    }
    Gate& gate;
    std::mutex mutex;
    std::thread::id reader;
};

struct TestServer : SmtpsServer {
    using SmtpsServer::SmtpsServer;
    void start() override {}
};

template <typename T> T get(std::future<T>& future) {
    assert(future.wait_for(5s) == std::future_status::ready);
    return future.get();
}

void owner(MockConnection* wire, std::function<void()> task) {
    std::promise<void> done;
    auto future = done.get_future();
    wire->executor()->post([&done, task = std::move(task)] { task(); done.set_value(); });
    get(future);
}

void run(Scenario scenario) {
    const auto root = test_support::temp_root() /
        ("smtp-local-commit-" + std::to_string(::getpid()) + "-" + std::to_string(static_cast<int>(scenario)));
    auto io = std::make_shared<pr::IOThreadPool>(1);
    auto workers = std::make_shared<pr::WorkerThreadPool>(1);
    io->start();
    workers->start();
    pr::LocalStorageRuntimeConfig runtime_config;
    runtime_config.commit_threads = 1;
    auto runtime = std::make_shared<pr::LocalStorageRuntime>(runtime_config);
    Gate gate;
    gate.fail = scenario == Scenario::Failure;
    auto entered = gate.entered.get_future();
    auto storage = std::make_shared<GatedStorage>(root, runtime, gate);
    auto router = std::make_shared<router::StaticShardRouter>(std::vector<std::pair<std::string, int>>{}, 0,
        std::vector<std::shared_ptr<DBPool>>{}, std::vector<std::shared_ptr<storage::IStorageProvider>>{storage});
    ServerConfig config;
    config.perf_mode = true;
    config.apply_perf_mode();
    config.use_database = false;
    config.listeners.clear();
    config.log_to_console = false;
    config.log_to_file = false;
    auto server = std::make_shared<TestServer>(config, io, workers, router);
    auto fsm = std::make_shared<TraditionalSmtpsFsm<MockConnection>>(io, workers, nullptr, router);
    auto connection = std::make_unique<MockConnection>();
    auto* wire = connection.get();
    wire->set_deferred_read(true);
    wire->start_executor();
    auto session = std::make_shared<SmtpsSession<MockConnection>>(server.get(), std::move(connection), fsm);
    std::string body;
    std::thread::id owner_thread;
    owner(wire, [&] {
        owner_thread = std::this_thread::get_id();
        auto* context = static_cast<SmtpsContext*>(session->get_context());
        context->sender_address = "sender@test.local";
        context->recipient_addresses = {"recipient@test.local"};
        session->set_current_state(static_cast<int>(SmtpsState::IN_MESSAGE));
        session->create_mail_on_data_command();
        session->parse_smtp_command("Subject: test\r\n\r\nhello\r\n.\r\n");
        body = session->get_mail()->body_path;
        fsm->process_event(session, SmtpsEvent::DATA_END);
    });
    get(entered);
    assert(runtime->snapshot().commits_running == 1);
    assert(wire->written().find("250 OK") == std::string::npos);
    // This executor pulse must complete while fsync remains blocked on its gate.
    owner(wire, [] {});

    std::uint64_t replacement_id = 0;
    if (scenario == Scenario::Closed) owner(wire, [&] { session->close(); });
    if (scenario == Scenario::Reset) owner(wire, [&] {
        session->discard_current_mail();
        auto* context = static_cast<SmtpsContext*>(session->get_context());
        context->sender_address = "new@test.local";
        context->recipient_addresses = {"new-recipient@test.local"};
        session->create_mail_on_data_command();
        replacement_id = session->get_mail()->id;
    });
    gate.release.set_value();
    runtime->drain();
    // Drain owner-posted protocol settlement/write initiation as well as the
    // storage completion. Each barrier observes work enqueued by its predecessor.
    for (int i = 0; i < 4; ++i) owner(wire, [] {});
    const auto response = wire->written();
    if (scenario == Scenario::Success) {
        assert(response == "250 OK\r\n");
        assert(std::filesystem::exists(body));
        std::lock_guard<std::mutex> lock(storage->mutex);
        assert(storage->reader == owner_thread);
    } else if (scenario == Scenario::Failure) {
        assert(response.find("451 ") == 0 && response.find("250 OK") == std::string::npos);
        assert(!std::filesystem::exists(body));
    } else {
        assert(response.empty());
        assert(!std::filesystem::exists(body));
        if (scenario == Scenario::Reset) owner(wire, [&] {
            assert(session->get_mail() && session->get_mail()->id == replacement_id);
        });
    }
    owner(wire, [&] { session->discard_current_mail(); session->close(); });
    runtime->shutdown();
    assert(runtime->snapshot().commits_outstanding == 0);
    assert(runtime->snapshot().uncommitted_bytes == 0);
    finish_session(session);
    workers->stop();
    io->stop();
    fsm.reset();
    server.reset();
    std::filesystem::remove_all(root);
}
} // namespace

int main() {
    run(Scenario::Success);
    run(Scenario::Closed);
    run(Scenario::Reset);
    run(Scenario::Failure);
    std::cout << "smtp_local_commit_test: 4 cases passed\n";
}
