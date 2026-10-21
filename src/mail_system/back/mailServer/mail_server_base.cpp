#include "mail_system/back/mailServer/mail_server_base.h"
#include <stdexcept>

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
    if (!m_metricsServer) return;
    if (m_shardRouter) {
        size_t n = m_shardRouter->shard_count();
        for (size_t i = 0; i < n; ++i) {
            pr::MetricsServer::LabelMap labels{{"shard", std::to_string(i)}};
            auto db = m_shardRouter->get_db_pool(i);
            if (db) {
                m_metricsServer->set_gauge("protorelay_db_pool_size", labels, db->get_pool_size());
                m_metricsServer->set_gauge("protorelay_db_available", labels, db->get_available_connections());
                m_metricsServer->set_gauge("protorelay_db_active", labels, db->get_active_connections());
                m_metricsServer->set_gauge("protorelay_db_pool_max", labels, db->get_max_pool_size());
            }
            auto st = m_shardRouter->get_storage(i);
            m_metricsServer->set_gauge("protorelay_storage_ready", labels, st ? 1 : 0);
        }
    }
}

} // namespace mail_system
