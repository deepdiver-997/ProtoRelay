#ifndef MAIL_SYSTEM_CARES_DNS_RESOLVER_H
#define MAIL_SYSTEM_CARES_DNS_RESOLVER_H

#include "mail_system/back/outbound/dns_resolver.h"

#include <ares.h>
#include <atomic>
#include <chrono>
#include <mutex>

namespace mail_system {
namespace outbound {

class CaresDnsResolver : public IDnsResolver {
public:
    CaresDnsResolver();
    ~CaresDnsResolver() override;

    std::vector<MxRecord> resolve_mx(const std::string& domain) override;
    std::vector<std::string> resolve_host_addresses(const std::string& host) override;

private:
    bool init_channel_locked();
    void destroy_channel_locked();
    bool run_event_loop_locked(std::atomic<bool>& done,
                               std::chrono::milliseconds timeout);

private:
    ares_channel channel_{nullptr};
    bool library_inited_{false};
    std::mutex mutex_;
};

} // namespace outbound
} // namespace mail_system

#endif // MAIL_SYSTEM_CARES_DNS_RESOLVER_H
