#include "mail_system/back/mailServer/mail_server_base.h"
#include "framework/storage/local_file_storage_provider.h"
#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace mail_system {
MailServerBase::MailServerBase(const Config& config, std::shared_ptr<pr::ThreadPoolBase> io,
                               std::shared_ptr<pr::ThreadPoolBase> workers,
                               std::shared_ptr<router::IShardRouter> router)
    : pr::ServerBase(config, std::move(io), std::move(workers)),
      m_config(std::make_shared<Config>(config)), m_shardRouter(std::move(router)),
      m_domain(config.system_domain.empty() ? "example.com" : config.system_domain) {
    if (!m_shardRouter) throw std::invalid_argument("MailServerBase: shard router is required");
}

MailServerBase::~MailServerBase() {
    // Metrics render/reload callbacks reference mail members. Quiesce them while
    // those members still exist, even for a paused or maintenance-only server.
    // Shared application runtimes remain owned by the composition root.
    try { stop_metrics_server(); } catch (...) {}
}

void MailServerBase::increment_mails_accepted() {
    mails_accepted_total_.fetch_add(1, std::memory_order_relaxed);
    push_metric_counter("protorelay_mails_accepted_total", {}, 1);
}

bool MailServerBase::reload_config(const std::string& filename) {
    m_configFilePath = filename;
    Config replacement = *std::atomic_load(&m_config);
    try {
        if (!replacement.loadFromFile(filename) || !validate_reload_structure(replacement)) return false;
        replacement.storage.local.runtime_config.validate();
        if (replacement.storage.local.runtime_config !=
            std::atomic_load(&m_config)->storage.local.runtime_config) {
            LOG_SERVER_WARN("Config reload rejected: storage.local commit_mode/commit_threads/"
                            "max_pending_commits/max_pending_commit_bytes are startup-only; requires restart");
            return false;
        }
    } catch (const std::exception& error) {
        LOG_SERVER_ERROR("Config reload failed: {}", error.what());
        return false;
    }
    auto snapshot = std::make_shared<Config>(std::move(replacement));
    std::atomic_store(&m_config, snapshot);
    pr::Logger::get_instance().set_level(pr::Logger::string_to_level(snapshot->log_level));
    LOG_SERVER_INFO("Config reloaded: log_level={}, auth={}", snapshot->log_level,
                    inbound_auth_policy_to_string(snapshot->inbound_auth_policy));
    return true;
}

void MailServerBase::refresh_metrics() {
    std::lock_guard<std::mutex> lock(local_storage_metrics_mutex_);
    auto metrics = m_metricsServer;
    if (!metrics) return;
    if (local_storage_metrics_registry_.lock() != metrics) {
        local_storage_metrics_registry_ = metrics;
        local_storage_metrics_last_.clear();
    }
    // Only local stream write/commit accounting: not all disk activity, replica
    // traffic, mmap, sidecars or kernel dirty-page statistics.
    using Snapshot = pr::LocalStorageRuntimeSnapshot;
    struct Field { const char* name; uint64_t Snapshot::*value; };
    static constexpr Field counters[] = {
        {"protorelay_local_storage_writes_total", &Snapshot::writes_total},
        {"protorelay_local_storage_writes_completed_total", &Snapshot::writes_completed},
        {"protorelay_local_storage_writes_failed_total", &Snapshot::writes_failed},
        {"protorelay_local_storage_write_bytes_requested_total", &Snapshot::write_bytes_requested},
        {"protorelay_local_storage_write_bytes_written_total", &Snapshot::write_bytes_written},
        {"protorelay_local_storage_write_elapsed_count", &Snapshot::write_elapsed_count},
        {"protorelay_local_storage_commits_accepted_total", &Snapshot::commits_accepted},
        {"protorelay_local_storage_commits_completed_total", &Snapshot::commits_completed},
        {"protorelay_local_storage_commits_failed_total", &Snapshot::commits_failed},
        {"protorelay_local_storage_commits_rejected_total", &Snapshot::commits_rejected},
        {"protorelay_local_storage_commit_dispatch_failures_total", &Snapshot::commit_dispatch_failures},
        {"protorelay_local_storage_commit_callback_exceptions_total", &Snapshot::commit_callback_exceptions},
        {"protorelay_local_storage_commit_queue_elapsed_count", &Snapshot::queue_elapsed_count},
        {"protorelay_local_storage_commit_service_elapsed_count", &Snapshot::service_elapsed_count},
    };
    static constexpr Field gauges[] = {
        {"protorelay_local_storage_writes_in_flight", &Snapshot::writes_in_flight},
        {"protorelay_local_storage_write_bytes_in_flight", &Snapshot::write_bytes_in_flight},
        {"protorelay_local_storage_uncommitted_bytes", &Snapshot::uncommitted_bytes},
        {"protorelay_local_storage_commits_queued", &Snapshot::commits_queued},
        {"protorelay_local_storage_commit_bytes_queued", &Snapshot::commit_bytes_queued},
        {"protorelay_local_storage_commits_running", &Snapshot::commits_running},
        {"protorelay_local_storage_commit_bytes_running", &Snapshot::commit_bytes_running},
        {"protorelay_local_storage_commits_completion_pending", &Snapshot::commits_completion_pending},
        {"protorelay_local_storage_commit_bytes_completion_pending", &Snapshot::commit_bytes_completion_pending},
        {"protorelay_local_storage_commits_outstanding", &Snapshot::commits_outstanding},
        {"protorelay_local_storage_commit_bytes_outstanding", &Snapshot::commit_bytes_outstanding},
    };
    std::unordered_set<const pr::LocalStorageRuntime*> seen;
    std::map<std::string, uint64_t> totals;
    if (m_shardRouter) {
        size_t n = m_shardRouter->shard_count();
        for (size_t i = 0; i < n; ++i) {
            pr::MetricsServer::LabelMap labels{{"shard", std::to_string(i)}};
            auto db = m_shardRouter->get_db_pool(i);
            if (db) {
                metrics->set_gauge("protorelay_db_pool_size", labels, db->get_pool_size());
                metrics->set_gauge("protorelay_db_available", labels, db->get_available_connections());
                metrics->set_gauge("protorelay_db_active", labels, db->get_active_connections());
                metrics->set_gauge("protorelay_db_pool_max", labels, db->get_max_pool_size());
            }
            auto st = m_shardRouter->get_storage(i);
            metrics->set_gauge("protorelay_storage_ready", labels, st ? 1 : 0);
            auto local = std::dynamic_pointer_cast<pr::storage::LocalFileStorageProvider>(st);
            if (!local) continue;
            auto runtime = local->local_storage_runtime();
            if (!runtime || !seen.insert(runtime.get()).second) continue;
            const auto current = runtime->snapshot();
            auto& previous = local_storage_metrics_last_[runtime.get()];
            for (const auto& field : counters) {
                const auto value = current.*field.value;
                const auto last = previous.*field.value;
                metrics->inc_counter(field.name, {}, value >= last ? value - last : value);
            }
            auto elapsed = [&](const char* name, std::chrono::nanoseconds value,
                               std::chrono::nanoseconds last) {
                metrics->inc_counter(name, {}, static_cast<uint64_t>((value >= last ? value - last : value).count()));
            };
            elapsed("protorelay_local_storage_write_elapsed_nanoseconds_total",
                    current.write_elapsed_total, previous.write_elapsed_total);
            elapsed("protorelay_local_storage_commit_queue_elapsed_nanoseconds_total",
                    current.queue_elapsed_total, previous.queue_elapsed_total);
            elapsed("protorelay_local_storage_commit_service_elapsed_nanoseconds_total",
                    current.service_elapsed_total, previous.service_elapsed_total);
            for (const auto& field : gauges) totals[field.name] += current.*field.value;
            ++totals["protorelay_local_storage_runtimes"];
            totals["protorelay_local_storage_admission_open_runtimes"] += current.admission_open ? 1 : 0;
            totals["protorelay_local_storage_commit_threads"] += runtime->config().commit_threads;
            totals["protorelay_local_storage_max_pending_commits"] += runtime->config().max_pending_commits;
            totals["protorelay_local_storage_max_pending_commit_bytes"] += runtime->config().max_pending_commit_bytes;
            auto maximum = [&](const char* name, std::chrono::nanoseconds value) {
                auto& total = totals[name];
                total = std::max(total, static_cast<uint64_t>(value.count()));
            };
            maximum("protorelay_local_storage_write_elapsed_nanoseconds_max", current.write_elapsed_max);
            maximum("protorelay_local_storage_commit_queue_elapsed_nanoseconds_max", current.queue_elapsed_max);
            maximum("protorelay_local_storage_commit_service_elapsed_nanoseconds_max", current.service_elapsed_max);
            maximum("protorelay_local_storage_commit_oldest_queue_age_nanoseconds", current.oldest_queue_age);
            previous = current;
        }
    }
    for (const auto& [name, value] : totals) metrics->set_gauge(name, {}, static_cast<double>(value));
}

} // namespace mail_system
