#ifndef PR_FRAMEWORK_NET_NATIVE_POLLER_H
#define PR_FRAMEWORK_NET_NATIVE_POLLER_H
// Native (non-Asio) readiness poller: a deliberately small wait interface with
// two backends — kqueue (macOS/BSD) and POSIX poll (portable fallback). epoll
// and select are intentionally absent in v1; epoll slots in behind this same
// interface later, select is strictly weaker (FD_SETSIZE).
//
// Semantics are unified to LEVEL-TRIGGERED readiness. Backends fold exceptional
// conditions into the two ordinary bits so that a consumer only ever reacts by
// attempting the pending syscall:
//   poll    POLLIN -> readable, POLLOUT -> writable,
//           POLLHUP -> readable|writable (read drains remaining bytes then
//           returns 0 = EOF; write surfaces EPIPE), POLLERR -> readable|writable
//           (syscall reports errno), POLLNVAL is dropped and the fd is
//           deregistered inside the poller (stale registration).
//   kqueue  EVFILT_READ -> readable (EV_EOF still readable: remaining data may
//           drain before the 0-byte EOF), EVFILT_WRITE -> writable (EV_EOF
//           writable: write() surfaces the disconnect), EV_ERROR ->
//           readable|writable.
// A wait must preserve its deadline across EINTR: the timeout is an absolute
// deadline, never restarted by an interrupted syscall.
#include <memory>
#include <string>
#include <vector>

namespace pr::net::native {

struct Ready {
    static constexpr unsigned none = 0;
    static constexpr unsigned readable = 1u << 0;
    static constexpr unsigned writable = 1u << 1;
};

struct PollEvent {
    int fd = -1;
    unsigned ready = Ready::none;
};

// One poller per event-loop thread; none of the calls are thread-safe. Interest
// uses the same two bits as events. add() on an already-registered fd or
// mod()/remove() on an unknown fd return false instead of throwing.
class Poller {
public:
    virtual ~Poller() = default;
    virtual bool add(int fd, unsigned events) = 0;
    virtual bool mod(int fd, unsigned events) = 0;
    virtual void remove(int fd) = 0;
    // timeout_ms: -1 blocks indefinitely, 0 polls, otherwise an upper bound on
    // the blocking time. Ready fds are appended to out (cleared first).
    virtual void wait(int timeout_ms, std::vector<PollEvent>& out) = 0;
};

enum class PollerBackend { kqueue, poll };

// kqueue on Apple/BSD, poll elsewhere.
PollerBackend default_poller_backend();
// Compile-time availability: kqueue only where it is compiled in.
bool backend_available(PollerBackend backend);
std::string backend_name(PollerBackend backend);
// Inverse of backend_name; unknown names yield the platform default.
PollerBackend backend_from_name(const std::string& name);
// nullptr when the backend is not compiled into this binary.
std::unique_ptr<Poller> make_poller(PollerBackend backend);
// Backend constructors; poller_kqueue.cpp/poller_poll.cpp define these.
std::unique_ptr<Poller> make_kqueue_poller();
std::unique_ptr<Poller> make_poll_poller();
// Every backend this binary can actually run (used by tests to cover each path).
std::vector<PollerBackend> available_backends();
// PROTORELAY_NATIVE_POLLER=kqueue|poll forces a backend; anything else (unset,
// empty, unknown, or unavailable on this platform) falls back to the default.
std::unique_ptr<Poller> make_poller_from_env(std::string* chosen = nullptr);

} // namespace pr::net::native
#endif
