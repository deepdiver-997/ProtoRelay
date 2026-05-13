#ifndef MAIL_SYSTEM_DNS_RESOLVER_H
#define MAIL_SYSTEM_DNS_RESOLVER_H

#include <cstdint>
#include <string>
#include <vector>

namespace mail_system {
namespace outbound {

struct MxRecord {
    std::string host;
    std::uint16_t priority{0};
};

class IDnsResolver {
public:
    virtual ~IDnsResolver() = default;

    // Resolve MX records for a domain, sorted by ascending priority.
    virtual std::vector<MxRecord> resolve_mx(const std::string& domain) = 0;

    // Resolve hostname to a list of numeric IP addresses (IPv4/IPv6).
    virtual std::vector<std::string> resolve_host_addresses(const std::string& host) = 0;

    // Resolve TXT records for a domain.
    virtual std::vector<std::string> resolve_txt(const std::string& domain) = 0;

    // Resolve PTR (reverse DNS) records for an IPv4/IPv6 address.
    virtual std::vector<std::string> resolve_ptr(const std::string& ip) = 0;

    // 优先走缓存，未命中才真正查询（子类可覆盖；默认无缓存直接查）
    virtual std::vector<std::string> resolve_ptr_cached(const std::string& ip) {
        return resolve_ptr(ip);
    }
};

} // namespace outbound
} // namespace mail_system

#endif // MAIL_SYSTEM_DNS_RESOLVER_H
