// epoll backend (Linux). Registered interest never carries EPOLLET, so
// readiness is LEVEL-TRIGGERED like the poll and kqueue backends: as long as
// data remains readable (or space writable) the fd keeps reporting. The two
// always-on conditions fold into the ordinary bits exactly like POLLHUP and
// POLLERR do in poller_poll.cpp — read drains the remaining bytes and then
// returns 0 = EOF; a read or write surfaces the real condition (data, EOF or
// errno) through its return value. Per man 7 epoll, EPOLLHUP/EPOLLERR need no
// subscription and always show up in epoll_wait output, matching how poll
// reports POLLHUP/POLLERR for descriptors registered with events == 0.
#include "framework/net/native/poller.h"

#if !defined(__linux__)
#error "poller_epoll.cpp requires Linux"
#else
#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <unordered_set>
#include <vector>

namespace pr::net::native {
namespace {
constexpr unsigned kInitialEventCapacity = 64;

class EpollPoller final : public Poller {
public:
    EpollPoller() : descriptor_(::epoll_create1(EPOLL_CLOEXEC)) {}

    ~EpollPoller() override {
        if (descriptor_ >= 0) ::close(descriptor_);
    }

    EpollPoller(const EpollPoller&) = delete;
    EpollPoller& operator=(const EpollPoller&) = delete;

    bool add(int fd, unsigned events) override {
        if (fd < 0 || descriptor_ < 0 || registered_.count(fd)) return false;
        epoll_event event{};
        event.events = interest_mask(events); // no EPOLLET, no EPOLLRDHUP:
                                              // EPOLLIN alone keeps reporting
                                              // read() == 0 at EOF level-wise
        event.data.fd = fd;
        // ADD, never MOD: a duplicate registration must fail with EEXIST even
        // if the local index ever disagreed with the kernel.
        if (::epoll_ctl(descriptor_, EPOLL_CTL_ADD, fd, &event) != 0) return false;
        registered_.insert(fd);
        return true;
    }

    bool mod(int fd, unsigned events) override {
        if (!registered_.count(fd)) return false;
        epoll_event event{};
        event.events = interest_mask(events);
        event.data.fd = fd;
        // MOD with an empty mask is legal in epoll and mirrors poll's
        // events == 0: the fd stays registered, ordinary readiness stops
        // reporting (EPOLLERR/EPOLLHUP still would, as in poll).
        return ::epoll_ctl(descriptor_, EPOLL_CTL_MOD, fd, &event) == 0;
    }

    void remove(int fd) override {
        if (registered_.erase(fd) == 0) return; // unknown or already removed
        epoll_event event{}; // pre-2.6.9 kernels required a non-null argument
        // DEL is best effort: ENOENT only means the kernel already detached
        // the fd (close() removes it from every interest list). The local
        // entry is dropped either way, so remove() stays re-entrant and the
        // fd number may be reused freely afterwards.
        ::epoll_ctl(descriptor_, EPOLL_CTL_DEL, fd, &event);
    }

    void wait(int timeout_ms, std::vector<PollEvent>& out) override {
        out.clear();
        if (descriptor_ < 0) return;
        // The deadline is absolute so an EINTR interruption cannot extend the
        // caller's wait (same treatment as poller_poll.cpp).
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
        long long remaining = timeout_ms;
        int count = 0;
        for (;;) {
            const int milliseconds = remaining < 0 ? -1 :
                static_cast<int>(remaining > INT_MAX ? INT_MAX : remaining);
            count = ::epoll_wait(descriptor_, events_.data(),
                                 static_cast<int>(events_.size()), milliseconds);
            if (count >= 0) break;
            if (errno != EINTR) return; // unknown epoll failure: report nothing
            if (timeout_ms >= 0) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) return;
                remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            }
        }
        // epoll reports an fd at most once per batch; the merge keeps the
        // one-PollEvent-per-fd invariant even if that ever changed.
        for (int i = 0; i < count; ++i) {
            const epoll_event& event = events_[i];
            unsigned ready = Ready::none;
            if (event.events & EPOLLIN) ready |= Ready::readable;
            if (event.events & EPOLLOUT) ready |= Ready::writable;
            // Hangup and error fold into the ordinary bits: the consumer's
            // next read()/write() reports the real condition (data, EOF or
            // errno) — identical to POLLHUP/POLLERR in poller_poll.cpp.
            if (event.events & EPOLLHUP) ready |= Ready::readable | Ready::writable;
            if (event.events & EPOLLERR) ready |= Ready::readable | Ready::writable;
            if (ready == Ready::none) continue;
            const int fd = event.data.fd;
            bool merged = false;
            for (auto& item : out) {
                if (item.fd == fd) { item.ready |= ready; merged = true; break; }
            }
            if (!merged) out.push_back(PollEvent{fd, ready});
        }
        // More ready fds than maxevents keeps the rest queued: level-triggered
        // interest re-reports them on the next wait.
    }

private:
    static uint32_t interest_mask(unsigned events) {
        uint32_t mask = 0;
        if (events & Ready::readable) mask |= EPOLLIN;
        if (events & Ready::writable) mask |= EPOLLOUT;
        return mask;
    }

    int descriptor_ = -1;
    std::vector<epoll_event> events_ = std::vector<epoll_event>(kInitialEventCapacity);
    std::unordered_set<int> registered_; // fds with a live kernel registration
};
} // namespace

std::unique_ptr<Poller> make_epoll_poller() { return std::make_unique<EpollPoller>(); }
} // namespace pr::net::native
#endif
