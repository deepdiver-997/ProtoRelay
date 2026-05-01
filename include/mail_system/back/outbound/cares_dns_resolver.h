#ifndef MAIL_SYSTEM_CARES_DNS_RESOLVER_H
#define MAIL_SYSTEM_CARES_DNS_RESOLVER_H

#include "mail_system/back/outbound/dns_resolver.h"

#include <ares.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace mail_system {
namespace outbound {

class CaresDnsResolver : public IDnsResolver {
public:
    CaresDnsResolver();
    ~CaresDnsResolver() override;

    std::vector<MxRecord> resolve_mx(const std::string& domain) override;
    std::vector<std::string> resolve_host_addresses(const std::string& host) override;
    std::vector<std::string> resolve_txt(const std::string& domain) override;

    // 优先走缓存，未命中才真正查询（适合 IO 线程快速调用）
    std::vector<std::string> resolve_txt_cached(const std::string& domain);

private:
    bool init_channel_locked();
    void destroy_channel_locked();
    bool run_event_loop_locked(std::atomic<bool>& done,
                               std::chrono::milliseconds timeout);

    struct TxtCacheEntry {
        std::vector<std::string> records;
        std::chrono::steady_clock::time_point expiry;
    };

private:
    ares_channel channel_{nullptr};
    bool library_inited_{false};
    std::mutex mutex_;

    std::mutex cache_mutex_;
    std::unordered_map<std::string, TxtCacheEntry> txt_cache_;
};

} // namespace outbound
} // namespace mail_system

#endif // MAIL_SYSTEM_CARES_DNS_RESOLVER_H
