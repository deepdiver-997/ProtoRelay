// kqueue backend (macOS/BSD). Filters are added WITHOUT EV_CLEAR, which makes
// them level-triggered like poll: as long as data remains readable (or space
// writable) the filter keeps reporting. EV_EOF marks a peer shutdown and folds
// into the ordinary readiness bits — read drains what is left and then returns
// 0; write surfaces the disconnect through its errno.
#include "framework/net/native/poller.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <unordered_set>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>

#if !defined(__APPLE__) && !defined(__FreeBSD__) && !defined(__OpenBSD__) && !defined(__NetBSD__)
#error "poller_kqueue.cpp requires a kqueue platform"
#endif

namespace pr::net::native {
namespace {
constexpr unsigned kInitialEventCapacity = 64;

class KqueuePoller final : public Poller {
public:
    KqueuePoller() : descriptor_(::kqueue()) {}

    ~KqueuePoller() override {
        if (descriptor_ >= 0) ::close(descriptor_);
    }

    KqueuePoller(const KqueuePoller&) = delete;
    KqueuePoller& operator=(const KqueuePoller&) = delete;

    bool add(int fd, unsigned events) override {
        if (fd < 0 || descriptor_ < 0 || registered_.count(fd)) return false;
        if (!apply(changes_for(fd, events, EV_ADD | EV_RECEIPT), nullptr)) return false;
        registered_.insert(fd);
        return true;
    }

    bool mod(int fd, unsigned events) override {
        if (!registered_.count(fd)) return false;
        std::vector<struct kevent> changes;
        // Filters to enable are re-added (updates an existing filter in place);
        // filters to drop are deleted. A delete may report ENOENT when that
        // filter was never registered — that is the desired end state.
        if (events & Ready::readable)
            append(changes, fd, EVFILT_READ, EV_ADD | EV_RECEIPT, 0);
        else
            append(changes, fd, EVFILT_READ, EV_DELETE, 0);
        if (events & Ready::writable)
            append(changes, fd, EVFILT_WRITE, EV_ADD | EV_RECEIPT, 0);
        else
            append(changes, fd, EVFILT_WRITE, EV_DELETE, 0);
        return apply(std::move(changes), &fd);
    }

    void remove(int fd) override {
        if (registered_.erase(fd) == 0) return;
        std::vector<struct kevent> changes;
        append(changes, fd, EVFILT_READ, EV_DELETE, 0);
        append(changes, fd, EVFILT_WRITE, EV_DELETE, 0);
        // Removal is best effort; ENOENT for a never-registered filter is fine
        // and the registration entry is dropped regardless.
        std::vector<struct kevent> outcomes(changes.size());
        ::kevent(descriptor_, changes.data(), static_cast<int>(changes.size()),
                 outcomes.data(), static_cast<int>(outcomes.size()), nullptr);
    }

    void wait(int timeout_ms, std::vector<PollEvent>& out) override {
        out.clear();
        if (descriptor_ < 0) return;
        // Absolute deadline: an EINTR must not restart the caller's timeout.
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
        long long remaining = timeout_ms;
        int count = 0;
        for (;;) {
            timespec timeout{};
            timespec* timeout_pointer = nullptr;
            if (remaining >= 0) {
                timeout.tv_sec = static_cast<time_t>(remaining / 1000);
                timeout.tv_nsec = static_cast<long>((remaining % 1000) * 1000000LL);
                timeout_pointer = &timeout;
            }
            count = ::kevent(descriptor_, nullptr, 0, events_.data(),
                             static_cast<int>(events_.size()), timeout_pointer);
            if (count >= 0) break;
            if (errno != EINTR) return;
            if (timeout_ms >= 0) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) return;
                remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            }
        }
        // One fd can report on both filters in a single batch: merge the bits.
        for (int i = 0; i < count; ++i) {
            const struct kevent& event = events_[i];
            unsigned ready = Ready::none;
            if (event.filter == EVFILT_READ) ready |= Ready::readable;
            else if (event.filter == EVFILT_WRITE) ready |= Ready::writable;
            else continue;
            if (event.flags & (EV_ERROR | EV_EOF)) ready |= Ready::readable | Ready::writable;
            if (ready == Ready::none) continue;
            const int fd = static_cast<int>(event.ident);
            bool merged = false;
            for (auto& item : out) {
                if (item.fd == fd) { item.ready |= ready; merged = true; break; }
            }
            if (!merged) out.push_back(PollEvent{fd, ready});
        }
    }

private:
    static void append(std::vector<struct kevent>& changes, int fd, int16_t filter,
                       uint16_t flags, uint32_t fflags) {
        struct kevent change;
        EV_SET(&change, static_cast<uintptr_t>(fd), filter, flags, fflags, 0, nullptr);
        changes.push_back(change);
    }

    static std::vector<struct kevent> changes_for(int fd, unsigned events, uint16_t flags) {
        std::vector<struct kevent> changes;
        if (events & Ready::readable) append(changes, fd, EVFILT_READ, flags, 0);
        if (events & Ready::writable) append(changes, fd, EVFILT_WRITE, flags, 0);
        return changes;
    }

    // Applies a change batch. `delete_fd` tolerates ENOENT outcomes for deletes
    // of a filter that was not registered (mod() removing one of two filters).
    bool apply(std::vector<struct kevent>&& changes, const int* delete_fd) {
        if (changes.empty()) return true;
        std::vector<struct kevent> outcomes(changes.size());
        const int applied = ::kevent(descriptor_, changes.data(),
                                     static_cast<int>(changes.size()),
                                     outcomes.data(), static_cast<int>(outcomes.size()), nullptr);
        if (applied < 0) return false;
        for (int i = 0; i < applied; ++i) {
            if (!(outcomes[i].flags & EV_ERROR)) continue;
            const bool tolerated_delete = delete_fd != nullptr &&
                static_cast<int>(outcomes[i].ident) == *delete_fd &&
                static_cast<int>(outcomes[i].data) == ENOENT;
            if (!tolerated_delete) return false;
        }
        return true;
    }

    int descriptor_ = -1;
    std::vector<struct kevent> events_ = std::vector<struct kevent>(kInitialEventCapacity);
    std::unordered_set<int> registered_; // fds with at least one live filter
};
} // namespace

std::unique_ptr<Poller> make_kqueue_poller() { return std::make_unique<KqueuePoller>(); }
} // namespace pr::net::native
