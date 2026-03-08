#include "mail_system/back/outbound/cares_dns_resolver.h"

#include "mail_system/back/common/logger.h"

#include <ares.h>
#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <netinet/in.h>

namespace mail_system {
namespace outbound {
namespace {

constexpr std::chrono::milliseconds kDefaultDnsTimeout{5000};

struct MxQueryContext {
    std::vector<MxRecord> records;
    std::atomic<bool>* done{nullptr};
    int status{ARES_EDESTRUCTION};
};

struct AddrQueryContext {
    std::vector<std::string> addresses;
    std::atomic<bool>* done{nullptr};
    int status{ARES_EDESTRUCTION};
};

void mx_query_callback(void* arg, int status, int timeouts, unsigned char* abuf, int alen) {
    (void)timeouts;
    auto* ctx = static_cast<MxQueryContext*>(arg);
    if (!ctx) {
        return;
    }

    ctx->status = status;
    if (status != ARES_SUCCESS) {
        if (ctx->done) {
            ctx->done->store(true);
        }
        return;
    }

    struct ares_mx_reply* mx_reply = nullptr;

    // Keep compatibility on current c-ares while isolating deprecated call usage.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    const int parse_status = ares_parse_mx_reply(abuf, alen, &mx_reply);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

    if (parse_status != ARES_SUCCESS || !mx_reply) {
        if (ctx->done) {
            ctx->done->store(true);
        }
        return;
    }

    for (auto* it = mx_reply; it != nullptr; it = it->next) {
        if (!it->host || std::strlen(it->host) == 0) {
            continue;
        }
        ctx->records.push_back(MxRecord{it->host, static_cast<std::uint16_t>(it->priority)});
    }

    ares_free_data(mx_reply);

    if (ctx->done) {
        ctx->done->store(true);
    }
}

void addr_query_callback(void* arg, int status, int timeouts, struct ares_addrinfo* result) {
    (void)timeouts;
    auto* ctx = static_cast<AddrQueryContext*>(arg);
    if (!ctx) {
        return;
    }

    ctx->status = status;
    if (status == ARES_SUCCESS && result != nullptr) {
        for (auto* node = result->nodes; node != nullptr; node = node->ai_next) {
            if (!node->ai_addr) {
                continue;
            }

            char ip_buf[INET6_ADDRSTRLEN] = {0};
            if (node->ai_family == AF_INET) {
                auto* addr = reinterpret_cast<sockaddr_in*>(node->ai_addr);
                if (inet_ntop(AF_INET, &(addr->sin_addr), ip_buf, sizeof(ip_buf)) != nullptr) {
                    ctx->addresses.emplace_back(ip_buf);
                }
            } else if (node->ai_family == AF_INET6) {
                auto* addr6 = reinterpret_cast<sockaddr_in6*>(node->ai_addr);
                if (inet_ntop(AF_INET6, &(addr6->sin6_addr), ip_buf, sizeof(ip_buf)) != nullptr) {
                    ctx->addresses.emplace_back(ip_buf);
                }
            }
        }
    }

    if (result != nullptr) {
        ares_freeaddrinfo(result);
    }

    if (ctx->done) {
        ctx->done->store(true);
    }
}

} // namespace

CaresDnsResolver::CaresDnsResolver() {
    std::lock_guard<std::mutex> lock(mutex_);
    const int lib_status = ares_library_init(ARES_LIB_INIT_ALL);
    if (lib_status != ARES_SUCCESS) {
        LOG_OUTBOUND_WARN("DNS(MX): ares_library_init failed, error={}", ares_strerror(lib_status));
        return;
    }
    library_inited_ = true;
    if (!init_channel_locked()) {
        LOG_OUTBOUND_WARN("DNS(MX): init channel failed");
    }
}

CaresDnsResolver::~CaresDnsResolver() {
    std::lock_guard<std::mutex> lock(mutex_);
    destroy_channel_locked();
    if (library_inited_) {
        ares_library_cleanup();
        library_inited_ = false;
    }
}

bool CaresDnsResolver::init_channel_locked() {
    if (!library_inited_) {
        return false;
    }

    if (channel_ != nullptr) {
        return true;
    }

    ares_options options{};
    options.evsys = ARES_EVSYS_DEFAULT;
    int optmask = ARES_OPT_EVENT_THREAD;
    const int init_status = ares_init_options(&channel_, &options, optmask);
    if (init_status != ARES_SUCCESS || channel_ == nullptr) {
        LOG_OUTBOUND_WARN("DNS(MX): ares_init_options failed, error={}", ares_strerror(init_status));
        channel_ = nullptr;
        return false;
    }
    return true;
}

void CaresDnsResolver::destroy_channel_locked() {
    if (channel_ != nullptr) {
        ares_destroy(channel_);
        channel_ = nullptr;
    }
}

bool CaresDnsResolver::run_event_loop_locked(std::atomic<bool>& done,
                                             std::chrono::milliseconds timeout) {
    if (channel_ == nullptr) {
        return false;
    }

    const int timeout_ms = timeout.count() <= 0 ? -1 : static_cast<int>(timeout.count());
    const ares_status_t wait_status = ares_queue_wait_empty(channel_, timeout_ms);
    if (wait_status != ARES_SUCCESS) {
        return false;
    }

    return done.load();
}

std::vector<MxRecord> CaresDnsResolver::resolve_mx(const std::string& domain) {
    std::vector<MxRecord> records;
    if (domain.empty()) {
        return records;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!init_channel_locked()) {
        LOG_OUTBOUND_WARN("DNS(MX): channel unavailable, domain={}", domain);
        return records;
    }

    MxQueryContext ctx;
    std::atomic<bool> done{false};
    ctx.done = &done;

    // Keep compatibility with current c-ares package while containing deprecated entry point.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    ares_query(channel_, domain.c_str(), ns_c_in, ns_t_mx, &mx_query_callback, &ctx);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

    const bool loop_ok = run_event_loop_locked(done, kDefaultDnsTimeout);
    if (!loop_ok || ctx.status != ARES_SUCCESS) {
        LOG_OUTBOUND_WARN("DNS(MX): lookup failed, domain={}, status={}, reason={}",
                        domain,
                        ctx.status,
                        ares_strerror(ctx.status));
        ares_cancel(channel_);
        return records;
    }

    std::sort(ctx.records.begin(), ctx.records.end(), [](const MxRecord& a, const MxRecord& b) {
        if (a.priority != b.priority) {
            return a.priority < b.priority;
        }
        return a.host < b.host;
    });

    records = std::move(ctx.records);
    return records;
}

std::vector<std::string> CaresDnsResolver::resolve_host_addresses(const std::string& host) {
    std::vector<std::string> addresses;
    if (host.empty()) {
        return addresses;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!init_channel_locked()) {
        LOG_OUTBOUND_WARN("DNS(A/AAAA): channel unavailable, host={}", host);
        return addresses;
    }

    std::atomic<bool> done{false};
    AddrQueryContext ctx;
    ctx.done = &done;

    struct ares_addrinfo_hints hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    ares_getaddrinfo(channel_, host.c_str(), nullptr, &hints, &addr_query_callback, &ctx);

    const bool loop_ok = run_event_loop_locked(done, kDefaultDnsTimeout);
    if (!loop_ok || ctx.status != ARES_SUCCESS) {
        LOG_OUTBOUND_WARN("DNS(A/AAAA): lookup failed, host={}, status={}, reason={}",
                        host,
                        ctx.status,
                        ares_strerror(ctx.status));
        ares_cancel(channel_);
        return addresses;
    }

    std::sort(ctx.addresses.begin(), ctx.addresses.end());
    ctx.addresses.erase(std::unique(ctx.addresses.begin(), ctx.addresses.end()), ctx.addresses.end());
    addresses = std::move(ctx.addresses);
    return addresses;
}

} // namespace outbound
} // namespace mail_system
