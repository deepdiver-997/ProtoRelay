// POSIX poll backend: the universal level-triggered fallback. No FD_SETSIZE
// limit; capacity is bounded only by memory.
#include "framework/net/native/poller.h"

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <unordered_map>
#include <poll.h>
#include <unistd.h>

namespace pr::net::native {
namespace {
class PollPoller final : public Poller {
public:
    bool add(int fd, unsigned events) override {
        if (fd < 0 || index_.count(fd)) return false;
        index_.emplace(fd, descriptors_.size());
        descriptors_.push_back(pollfd{fd, interest_mask(events), 0});
        return true;
    }

    bool mod(int fd, unsigned events) override {
        const auto found = index_.find(fd);
        if (found == index_.end()) return false;
        descriptors_[found->second].events = interest_mask(events);
        return true;
    }

    void remove(int fd) override {
        const auto found = index_.find(fd);
        if (found == index_.end()) return;
        const std::size_t index = found->second;
        index_.erase(found);
        if (index != descriptors_.size() - 1) {
            descriptors_[index] = descriptors_.back();
            index_[descriptors_[index].fd] = index;
        }
        descriptors_.pop_back();
    }

    void wait(int timeout_ms, std::vector<PollEvent>& out) override {
        out.clear();
        // The deadline is absolute so an EINTR interruption cannot extend the
        // caller's wait (same treatment as the MariaDB readiness poller).
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
        long long remaining = timeout_ms;
        for (;;) {
            const int milliseconds = remaining < 0 ? -1 :
                static_cast<int>(remaining > INT_MAX ? INT_MAX : remaining);
            const int count = ::poll(descriptors_.data(), descriptors_.size(), milliseconds);
            if (count > 0) break;
            if (count == 0) return; // timeout with nothing ready
            if (errno != EINTR) return; // unknown poll failure: report nothing
            if (timeout_ms >= 0) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) return;
                remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            }
        }
        for (const auto& descriptor : descriptors_) {
            if (!descriptor.revents) continue;
            unsigned ready = Ready::none;
            if (descriptor.revents & POLLIN) ready |= Ready::readable;
            if (descriptor.revents & POLLOUT) ready |= Ready::writable;
            if (descriptor.revents & POLLNVAL) {
                // Stale registration for a closed fd: drop it here, otherwise
                // every subsequent wait would report it again and busy-spin.
                remove(descriptor.fd);
                continue;
            }
            // Hangup and error fold into the ordinary bits: the consumer's next
            // read()/write() reports the real condition (data, EOF or errno).
            if (descriptor.revents & (POLLHUP | POLLERR)) ready |= Ready::readable | Ready::writable;
            if (ready != Ready::none) out.push_back(PollEvent{descriptor.fd, ready});
        }
    }

private:
    static short interest_mask(unsigned events) {
        short mask = 0;
        if (events & Ready::readable) mask |= POLLIN;
        if (events & Ready::writable) mask |= POLLOUT;
        return mask;
    }

    std::vector<pollfd> descriptors_;
    std::unordered_map<int, std::size_t> index_;
};
} // namespace

std::unique_ptr<Poller> make_poll_poller() { return std::make_unique<PollPoller>(); }
} // namespace pr::net::native
