#pragma once

#include "framework/server_base.h"
#include "framework/net/dns_resolver.h"
#include "mail_system/back/server_config.h"
#include "mail_system/back/router/i_shard_router.h"

namespace mail_system {
class IMailboxCache;

// Mail resources belong to the application. The framework owns neither these
// resources nor the shared runtimes injected by bootstrap/the application main.
class MailServerBase : public pr::ServerBase {
public:
    using Config = mail_system::ServerConfig;
    MailServerBase(const Config& config, std::shared_ptr<pr::ThreadPoolBase> io,
                   std::shared_ptr<pr::ThreadPoolBase> workers,
                   std::shared_ptr<router::IShardRouter> router);
    ~MailServerBase() override;
    std::shared_ptr<const pr::ServerConfig> config_snapshot() const override {
        return std::atomic_load(&m_config);
    }
    bool reload_config(const std::string& filename) override;
    void refresh_metrics() override;
    void increment_mails_accepted();
    void set_mailbox_cache(std::shared_ptr<IMailboxCache> cache) { m_mailboxCache = std::move(cache); }
    std::shared_ptr<IMailboxCache> get_mailbox_cache() const { return m_mailboxCache; }
    std::shared_ptr<router::IShardRouter> get_shard_router() const { return m_shardRouter; }
    std::shared_ptr<pr::IDnsResolver> get_dns_resolver() const { return m_dnsResolver; }

    // m_config is the application's single published snapshot. Generic sessions
    // access the same snapshot through config_snapshot(), including after reload.
    std::shared_ptr<Config> m_config;
    std::shared_ptr<router::IShardRouter> m_shardRouter;
    std::shared_ptr<IMailboxCache> m_mailboxCache;
    std::shared_ptr<pr::IDnsResolver> m_dnsResolver;
    std::string m_domain;
    std::atomic<size_t> mails_accepted_total_{0};
};
} // namespace mail_system
