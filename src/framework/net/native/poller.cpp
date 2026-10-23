#include "framework/net/native/poller.h"
#include <cstdlib>
#include <cstring>

namespace pr::net::native {
namespace {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
constexpr bool kKqueueCompiled = true;
#else
constexpr bool kKqueueCompiled = false;
#endif
constexpr bool kPollCompiled = true; // POSIX poll is the universal fallback.
#if defined(__linux__)
constexpr bool kEpollCompiled = true;
#else
constexpr bool kEpollCompiled = false;
#endif

bool parse_backend(const char* text, PollerBackend& out) {
    if (!text || !*text) return false;
    if (std::strcmp(text, "kqueue") == 0) { out = PollerBackend::kqueue; return true; }
    if (std::strcmp(text, "poll") == 0) { out = PollerBackend::poll; return true; }
#if defined(__linux__)
    if (std::strcmp(text, "epoll") == 0) { out = PollerBackend::epoll; return true; }
#endif
    return false;
}
} // namespace

PollerBackend default_poller_backend() {
#if defined(__linux__)
    return PollerBackend::epoll; // epoll scales past poll's O(n) readiness scan
#else
    return kKqueueCompiled ? PollerBackend::kqueue : PollerBackend::poll;
#endif
}

bool backend_available(PollerBackend backend) {
    switch (backend) {
    case PollerBackend::kqueue: return kKqueueCompiled;
    case PollerBackend::poll: return kPollCompiled;
#if defined(__linux__)
    case PollerBackend::epoll: return kEpollCompiled;
#endif
    }
    return false;
}

std::string backend_name(PollerBackend backend) {
    switch (backend) {
    case PollerBackend::kqueue: return "kqueue";
    case PollerBackend::poll: return "poll";
#if defined(__linux__)
    case PollerBackend::epoll: return "epoll";
#endif
    }
    return "poll";
}

PollerBackend backend_from_name(const std::string& name) {
    PollerBackend parsed = default_poller_backend();
    parse_backend(name.c_str(), parsed);
    return parsed;
}

std::vector<PollerBackend> available_backends() {
    std::vector<PollerBackend> backends;
    if (kKqueueCompiled) backends.push_back(PollerBackend::kqueue);
    backends.push_back(PollerBackend::poll);
#if defined(__linux__)
    if (kEpollCompiled) backends.push_back(PollerBackend::epoll);
#endif
    return backends;
}

std::unique_ptr<Poller> make_poller(PollerBackend backend) {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    if (backend == PollerBackend::kqueue) return make_kqueue_poller();
#endif
#if defined(__linux__)
    if (backend == PollerBackend::epoll) return make_epoll_poller();
#endif
    if (backend == PollerBackend::poll) return make_poll_poller();
    return nullptr;
}

std::unique_ptr<Poller> make_poller_from_env(std::string* chosen) {
    const char* override_backend = std::getenv("PROTORELAY_NATIVE_POLLER");
    PollerBackend backend = default_poller_backend();
    if (parse_backend(override_backend, backend) && !backend_available(backend)) {
        // Requested a backend this binary does not contain: keep the default.
        backend = default_poller_backend();
    }
    if (chosen) *chosen = backend_name(backend);
    return make_poller(backend);
}

} // namespace pr::net::native
