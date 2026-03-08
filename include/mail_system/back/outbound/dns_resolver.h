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
};

} // namespace outbound
} // namespace mail_system

#endif // MAIL_SYSTEM_DNS_RESOLVER_H
