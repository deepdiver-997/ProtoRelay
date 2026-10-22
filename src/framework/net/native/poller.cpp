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

bool parse_backend(const char* text, PollerBackend& out) {
    if (!text || !*text) return false;
    if (std::strcmp(text, "kqueue") == 0) { out = PollerBackend::kqueue; return true; }
    if (std::strcmp(text, "poll") == 0) { out = PollerBackend::poll; return true; }
    return false;
}
} // namespace

PollerBackend default_poller_backend() {
    return kKqueueCompiled ? PollerBackend::kqueue : PollerBackend::poll;
}

bool backend_available(PollerBackend backend) {
    return backend == PollerBackend::kqueue ? kKqueueCompiled : kPollCompiled;
}

std::string backend_name(PollerBackend backend) {
    return backend == PollerBackend::kqueue ? "kqueue" : "poll";
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
    return backends;
}

std::unique_ptr<Poller> make_poller(PollerBackend backend) {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    if (backend == PollerBackend::kqueue) return make_kqueue_poller();
#else
    (void)backend; // kqueue is not compiled into this binary
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
