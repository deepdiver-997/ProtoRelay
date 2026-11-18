// Application composition-root tests: no database, listener or external service.
#undef NDEBUG
#include "test_temp_dir.h"
#include "mail_system/back/bootstrap.h"
#include "mail_system/back/db/null_db_pool.h"
#include "mail_system/back/mailServer/mail_server_base.h"
#include "framework/storage/local_file_storage_provider.h"

#include <cassert>
#include <fstream>
#include <future>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace {
using mail_system::ServerConfig;
namespace bootstrap = mail_system::bootstrap;

ServerConfig configuration(const std::string& name, bool shards = false) {
    ServerConfig config;
    config.use_database = false;
    config.io_thread_count = 1;
    config.worker_thread_count = 1;
    config.metrics_enabled = false;
    config.log_to_console = false;
    config.log_to_file = false;
    config.log_file = test_support::temp_path(name + ".log");
    config.intrusion_detection_enabled = false;
    config.intrusion_data_file = test_support::temp_path(name + "-intrusion.json");
    config.storage.local.mail_path = test_support::temp_path(name + "/mail");
    config.storage.local.attachment_path = test_support::temp_path(name + "/attachments");
    if (shards) {
        config.router_config.shard_count = 3;
        // An empty root uses the main provider; the other roots create distinct
        // provider objects, all of which must carry the exact same runtime.
        config.router_config.shards = {{},
            {"", test_support::temp_path(name + "/shard1")},
            {"", test_support::temp_path(name + "/shard2")}};
    }
    return config;
}

std::shared_ptr<pr::storage::LocalFileStorageProvider> local_provider(
        const bootstrap::Assembled& assembled, size_t shard = 0) {
    auto provider = std::dynamic_pointer_cast<pr::storage::LocalFileStorageProvider>(
        assembled.shard_router->get_storage(shard));
    assert(provider);
    return provider;
}

class TestServer : public mail_system::MailServerBase {
public:
    using MailServerBase::MailServerBase;
    void start() override {} // Intentionally never opens a listener.
    std::shared_ptr<pr::MetricsServer> replace_metrics_registry() {
        m_metricsServer = std::make_shared<pr::MetricsServer>(
            nullptr, 0, "127.0.0.1", nullptr, [this] { refresh_metrics(); });
        return m_metricsServer;
    }
protected:
    bool should_reject_connection(std::string&, const std::string& = "") const override {
        return false;
    }
};

uint64_t sample(const std::string& text, const std::string& name) {
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line); ) {
        if (line.compare(0, name.size() + 1, name + " ") == 0)
            return std::stoull(line.substr(name.size() + 1));
    }
    throw std::runtime_error("missing metric: " + name);
}

void assemble_sharing_and_isolation() {
    auto config = configuration("assembly", true);
    bootstrap::ShutdownScope shutdown;
    auto primary = bootstrap::assemble(config);
    shutdown.add_resources(primary);
    assert(primary.local_storage_runtime);
    assert(primary.local_storage_runtime->config() == config.storage.local.runtime_config);
    assert(primary.shard_router->shard_count() == 3);
    auto main = local_provider(primary);
    for (size_t shard = 0; shard != 3; ++shard) {
        assert(local_provider(primary, shard)->local_storage_runtime() == primary.local_storage_runtime);
        assert(std::dynamic_pointer_cast<mail_system::NullDBPool>(primary.shard_router->get_db_pool(shard)));
    }
    assert(main != local_provider(primary, 1));
    assert(local_provider(primary, 1) != local_provider(primary, 2));

    bootstrap::Options options;
    options.io_pool = primary.io_pool;
    options.worker_pool = primary.worker_pool;
    // Same paths and IO pool do NOT imply storage runtime sharing.
    auto independent = bootstrap::assemble(config, options);
    shutdown.add_resources(independent);
    assert(independent.local_storage_runtime != primary.local_storage_runtime);
    assert(local_provider(independent)->local_storage_runtime() == independent.local_storage_runtime);

    options.local_storage_runtime = primary.local_storage_runtime;
    auto shared = bootstrap::assemble(config, options);
    shutdown.add_resources(shared);
    assert(shared.local_storage_runtime == primary.local_storage_runtime);
    for (size_t shard = 0; shard != 3; ++shard)
        assert(local_provider(shared, shard)->local_storage_runtime() == primary.local_storage_runtime);

    for (int field = 0; field != 4; ++field) {
        auto mismatch = config;
        auto& runtime = mismatch.storage.local.runtime_config;
        switch (field) {
        case 0: runtime.commit_mode = pr::LocalCommitMode::Inline; break;
        case 1: ++runtime.commit_threads; break;
        case 2: ++runtime.max_pending_commits; break;
        case 3: ++runtime.max_pending_commit_bytes; break;
        }
        bool rejected = false;
        try { (void)bootstrap::assemble(mismatch, options); }
        catch (const std::invalid_argument& error) {
            rejected = std::string(error.what()).find("local_storage_runtime conflicts") != std::string::npos;
        }
        assert(rejected);
    }
    auto invalid = config;
    invalid.storage.local.runtime_config.max_pending_commits = 0;
    bool invalid_rejected = false;
    try { (void)bootstrap::assemble(invalid); }
    catch (const std::invalid_argument&) { invalid_rejected = true; }
    assert(invalid_rejected);

    // An individual provider's lifetime never closes a shared runtime.
    auto provider = std::make_unique<pr::storage::LocalFileStorageProvider>(
        std::vector<std::string>{}, primary.local_storage_runtime);
    provider.reset();
    assert(primary.local_storage_runtime->snapshot().admission_open);
    primary.shutdown_local_storage();
    assert(!shared.local_storage_runtime->snapshot().admission_open);
    assert(independent.local_storage_runtime->snapshot().admission_open);
    shutdown.shutdown(); // Shared resources are deduplicated and shutdown is idempotent.
    shutdown.shutdown();
}

void reload_startup_only() {
    auto config = configuration("reload");
    bootstrap::ShutdownScope shutdown;
    auto resources = bootstrap::assemble(config);
    shutdown.add_resources(resources);
    TestServer server(config, resources.io_pool, resources.worker_pool, resources.shard_router);
    const auto original = server.config_snapshot();
    const auto path = test_support::temp_path("local-runtime-reload.json");
    auto write = [&](const nlohmann::json& local) {
        std::ofstream output(path);
        output << nlohmann::json{{"log_level", "debug"}, {"storage", {{"local", local}}}};
    };
    for (const auto& local : {
            nlohmann::json{{"commit_mode", "inline"}},
            nlohmann::json{{"commit_threads", 3}},
            nlohmann::json{{"max_pending_commits", 65}},
            nlohmann::json{{"max_pending_commit_bytes", 64ull * 1024 * 1024 + 1}},
            nlohmann::json{{"commit_mode", "invalid"}},
            nlohmann::json{{"max_pending_commits", -1}}}) {
        write(local);
        assert(!server.reload_config(path));
        assert(server.config_snapshot() == original);
        assert(resources.local_storage_runtime->config() == config.storage.local.runtime_config);
    }
    write({{"mail_path", config.storage.local.mail_path},
           {"attachment_path", config.storage.local.attachment_path}});
    assert(server.reload_config(path));
    assert(server.config_snapshot()->log_level == "debug");
    assert(std::atomic_load(&server.m_config)->storage.local.runtime_config ==
           resources.local_storage_runtime->config());
}

void metrics_are_per_exporter_and_deduplicated() {
    auto config = configuration("metrics", true);
    bootstrap::ShutdownScope shutdown;
    auto resources = bootstrap::assemble(config);
    shutdown.add_resources(resources);
    TestServer first(config, resources.io_pool, resources.worker_pool, resources.shard_router);
    TestServer second(config, resources.io_pool, resources.worker_pool, resources.shard_router);
    auto first_registry = first.replace_metrics_registry();
    auto second_registry = second.replace_metrics_registry();

    pr::IoError error;
    auto stream = local_provider(resources)->open_write(config.storage.local.mail_path + "/metric.eml", error);
    assert(stream && stream->write_at(0, "hello", 5, error));
    assert(resources.local_storage_runtime->snapshot().uncommitted_bytes == 5);
    auto before = first_registry->scrape_text();
    assert(sample(before, "protorelay_local_storage_uncommitted_bytes") == 5);
    assert(sample(before, "protorelay_local_storage_runtimes") == 1); // Not 3 shards.
    auto executor = first.get_executor();
    std::promise<void> completed;
    auto ready = completed.get_future();
    stream->commit_async_on([executor](std::function<void()> completion) {
        executor->post(std::move(completion));
    }, [&completed](bool ok, const pr::IoError&) { assert(ok); completed.set_value(); });
    ready.get();
    resources.local_storage_runtime->drain();
    const auto once = first_registry->scrape_text();
    assert(sample(once, "protorelay_local_storage_writes_total") == 1);
    assert(sample(once, "protorelay_local_storage_write_bytes_written_total") == 5);
    assert(sample(once, "protorelay_local_storage_commits_accepted_total") == 1);
    assert(sample(once, "protorelay_local_storage_commits_completed_total") == 1);
    assert(sample(once, "protorelay_local_storage_commits_outstanding") == 0);
    assert(sample(once, "protorelay_local_storage_uncommitted_bytes") == 0);
    for (unsigned i = 0; i != 5; ++i) assert(first_registry->scrape_text() == once);
    assert(sample(second_registry->scrape_text(), "protorelay_local_storage_writes_total") == 1);
    assert(sample(second_registry->scrape_text(), "protorelay_local_storage_commits_completed_total") == 1);

    // A restarted exporter starts from the lifetime snapshot, not an old baseline.
    first_registry = first.replace_metrics_registry();
    assert(sample(first_registry->scrape_text(), "protorelay_local_storage_writes_total") == 1);
    auto aborted = local_provider(resources, 1)->open_write(
        test_support::temp_path("metrics/shard1/mail/abort.eml"), error);
    assert(aborted && aborted->write_at(0, "new", 3, error));
    aborted->abort();
    std::vector<std::future<std::string>> scrapes;
    for (unsigned i = 0; i != 4; ++i)
        scrapes.push_back(std::async(std::launch::async, [first_registry] { return first_registry->scrape_text(); }));
    for (auto& scrape : scrapes)
        assert(sample(scrape.get(), "protorelay_local_storage_writes_total") == 2);
    assert(sample(second_registry->scrape_text(), "protorelay_local_storage_writes_total") == 2);
    assert(sample(first_registry->scrape_text(), "protorelay_local_storage_write_bytes_written_total") == 8);
}

struct ShutdownProbe {
    std::promise<void> quiesced;
    std::atomic<bool> sessions_closed{false}, completion{false}, stopped{false}, destroyed{false};
};
class ShutdownProbeServer final : public TestServer {
public:
    ShutdownProbeServer(const ServerConfig& config, const bootstrap::Assembled& resources,
                        std::shared_ptr<ShutdownProbe> probe)
        : TestServer(config, resources.io_pool, resources.worker_pool, resources.shard_router),
          runtime_(resources.local_storage_runtime), probe_(std::move(probe)) {}
    ~ShutdownProbeServer() override {
        assert(probe_->completion && probe_->stopped);
        assert(!m_ioThreadPool->is_running() && !m_workerThreadPool->is_running());
        probe_->destroyed = true;
    }
    void quiesce_sessions() {
        assert(get_state() == pr::ServerState::Pausing);
        if (!probe_->sessions_closed.exchange(true)) probe_->quiesced.set_value();
    }
protected:
    void stop(pr::ServerState state = pr::ServerState::Pausing) override {
        assert(probe_->sessions_closed && probe_->completion);
        assert(runtime_->snapshot().commits_outstanding == 0);
        assert(!runtime_->snapshot().admission_open);
        assert(m_ioThreadPool->is_running() && m_workerThreadPool->is_running());
        probe_->stopped = true;
        MailServerBase::stop(state);
    }
private:
    std::shared_ptr<pr::LocalStorageRuntime> runtime_;
    std::shared_ptr<ShutdownProbe> probe_;
};

void shutdown_keeps_completion_executor_alive(bool fail_startup) {
    auto config = configuration(fail_startup ? "exception-shutdown" : "shutdown");
    auto probe = std::make_shared<ShutdownProbe>();
    bool exception_seen = false;
    try {
        // The slot outlives its guard (the application entries use global slots).
        std::unique_ptr<ShutdownProbeServer> server;
        bootstrap::ShutdownScope shutdown;
        shutdown.add_server(server);
        auto resources = bootstrap::assemble(config);
        shutdown.add_resources(resources);
        server = std::make_unique<ShutdownProbeServer>(config, resources, probe);
        auto executor = server->get_executor();
        auto quiesced = probe->quiesced.get_future().share();
        auto* live_server = server.get();
        assert(resources.local_storage_runtime->submit_commit(1,
            [quiesced](pr::IoError&) { quiesced.wait(); return true; },
            [probe, live_server](bool ok, const pr::IoError&) {
                assert(ok && probe->sessions_closed && !probe->destroyed);
                assert(live_server->get_state() == pr::ServerState::Pausing);
                probe->completion = true;
            }, [executor](std::function<void()> completion) { executor->post(std::move(completion)); }));
        if (fail_startup) throw std::runtime_error("simulated startup failure after assembly");
        shutdown.shutdown();
        assert(!server);
    } catch (const std::runtime_error&) {
        assert(fail_startup);
        exception_seen = true;
    }
    assert(exception_seen == fail_startup);
    assert(probe->sessions_closed && probe->completion && probe->stopped && probe->destroyed);
}
} // namespace

int main() {
    assemble_sharing_and_isolation();
    reload_startup_only();
    metrics_are_per_exporter_and_deduplicated();
    shutdown_keeps_completion_executor_alive(false);
    shutdown_keeps_completion_executor_alive(true);
    std::cout << "local_storage_bootstrap_test passed\n";
}
