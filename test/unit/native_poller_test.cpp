// Native poller contract tests, run against EVERY backend compiled into this
// binary (kqueue + poll on macOS, epoll + poll on Linux): level-triggered
// readiness, hangup folding, EINTR deadline preservation and the env override
// switch.
#undef NDEBUG
#include "framework/net/native/poller.h"

#include <cassert>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

using namespace pr::net::native;

namespace {

struct Pipe {
    int fds[2] = {-1, -1};
    Pipe() {
        assert(::pipe(fds) == 0);
        for (int fd : fds) {
            const int flags = ::fcntl(fd, F_GETFL, 0);
            ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
    }
    ~Pipe() {
        for (int fd : fds) ::close(fd);
    }
    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;
};

void write_byte(int fd, char value) { assert(::write(fd, &value, 1) == 1); }

// Level-triggered: readiness keeps firing until the data is drained, and
// removal silences a descriptor completely.
void level_triggered(PollerBackend backend) {
    auto poller = make_poller(backend);
    assert(poller);
    Pipe readable_pipe;
    Pipe writable_pipe;

    write_byte(readable_pipe.fds[1], 'x');
    assert(poller->add(readable_pipe.fds[0], Ready::readable));
    assert(poller->add(writable_pipe.fds[1], Ready::writable));

    std::vector<PollEvent> events;
    poller->wait(1000, events);
    assert(events.size() == 2);
    unsigned read_ready = 0, write_ready = 0;
    for (const auto& event : events) {
        if (event.fd == readable_pipe.fds[0]) read_ready = event.ready;
        if (event.fd == writable_pipe.fds[1]) write_ready = event.ready;
    }
    assert(read_ready == Ready::readable);
    assert(write_ready == Ready::writable);

    // Still ready without new input — the level-triggered contract.
    poller->wait(100, events);
    assert(events.size() == 2);

    // Draining the pipe silences it; the always-writable end stays ready.
    char sink = 0;
    assert(::read(readable_pipe.fds[0], &sink, 1) == 1);
    poller->wait(0, events);
    assert(events.size() == 1 && events[0].fd == writable_pipe.fds[1]);

    // mod() can retract interest; a retracted fd reports nothing.
    assert(poller->mod(writable_pipe.fds[1], 0));
    poller->wait(0, events);
    assert(events.empty());

    // mod() back restores it, add of a live fd fails, remove silences again.
    assert(poller->mod(writable_pipe.fds[1], Ready::writable));
    poller->wait(0, events);
    assert(events.size() == 1);
    assert(!poller->add(writable_pipe.fds[1], Ready::writable));
    assert(!poller->mod(-1, Ready::writable));
    poller->remove(writable_pipe.fds[1]);
    poller->remove(writable_pipe.fds[1]); // removing twice is tolerated
    poller->wait(0, events);
    assert(events.empty());
}

// POLLHUP / EV_EOF fold into the readable bit: the consumer observes EOF by
// reading the remaining bytes and then getting 0.
void hangup_folds_into_readable(PollerBackend backend) {
    auto poller = make_poller(backend);
    assert(poller);
    Pipe pipe;
    write_byte(pipe.fds[1], 'q');
    assert(poller->add(pipe.fds[0], Ready::readable));
    ::close(pipe.fds[1]); // peer hangup with one byte still buffered

    std::vector<PollEvent> events;
    poller->wait(1000, events);
    assert(events.size() == 1 && events[0].fd == pipe.fds[0]);
    assert(events[0].ready & Ready::readable);

    // Remaining data first...
    char sink = 0;
    assert(::read(pipe.fds[0], &sink, 1) == 1 && sink == 'q');
    // ...then the EOF as readable again.
    poller->wait(1000, events);
    assert(events.size() == 1 && (events[0].ready & Ready::readable));
    assert(::read(pipe.fds[0], &sink, 1) == 0);
}

// An interrupted wait must keep the caller's deadline: repeated SIGUSR1 cannot
// extend (or skip) the timeout (same rule as the MariaDB readiness poller).
void eintr_preserves_deadline(PollerBackend backend) {
    auto poller = make_poller(backend);
    assert(poller);

    struct sigaction action{};
    action.sa_handler = [](int) {};
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    ::sigaction(SIGUSR1, &action, nullptr);

    std::atomic<bool> stop{false};
    std::thread spammer([&stop] {
        while (!stop.load(std::memory_order_relaxed)) {
            ::kill(::getpid(), SIGUSR1);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    const auto started = std::chrono::steady_clock::now();
    std::vector<PollEvent> events;
    poller->wait(300, events); // nothing registered: pure deadline behavior
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    stop.store(true, std::memory_order_relaxed);
    spammer.join();

    assert(events.empty());
    // Interrupted retries must not shorten the wait below ~200ms of the asked
    // 300ms, and a livelock would blow far past 2s.
    assert(elapsed.count() >= 200);
    assert(elapsed.count() < 2500);
}

// A descriptor closed behind the poller's back must not produce endless
// events (poll would report POLLNVAL forever; kqueue drops the knote itself).
void closed_fd_is_dropped(PollerBackend backend) {
    auto poller = make_poller(backend);
    assert(poller);
    Pipe pipe;
    assert(poller->add(pipe.fds[0], Ready::readable));
    ::close(pipe.fds[0]);
    pipe.fds[0] = -1;
    std::vector<PollEvent> events;
    for (int round = 0; round < 3; ++round) {
        poller->wait(0, events);
        assert(events.empty());
    }
}

} // namespace

int main() {
    const auto backends = available_backends();
    assert(!backends.empty());
    for (const PollerBackend backend : backends) {
        level_triggered(backend);
        hangup_folds_into_readable(backend);
        eintr_preserves_deadline(backend);
        closed_fd_is_dropped(backend);
    }

    // PROTORELAY_NATIVE_POLLER forces a backend; unknown values fall back to
    // the platform default.
    std::string chosen;
    ::setenv("PROTORELAY_NATIVE_POLLER", "poll", 1);
    assert(make_poller_from_env(&chosen) && chosen == "poll");
#if defined(__linux__)
    ::setenv("PROTORELAY_NATIVE_POLLER", "epoll", 1);
    assert(make_poller_from_env(&chosen) && chosen == "epoll");
#else
    // epoll is not compiled in here — the enumerator does not exist, the name
    // parses as unknown and the request lands on the platform default.
    ::setenv("PROTORELAY_NATIVE_POLLER", "epoll", 1);
    assert(make_poller_from_env(&chosen) && chosen == backend_name(default_poller_backend()));
#endif
#ifdef __APPLE__
    ::setenv("PROTORELAY_NATIVE_POLLER", "kqueue", 1);
    assert(make_poller_from_env(&chosen) && chosen == "kqueue");
#endif
    ::setenv("PROTORELAY_NATIVE_POLLER", "definitely-not-a-backend", 1);
    assert(make_poller_from_env(&chosen) && chosen == backend_name(default_poller_backend()));
    ::unsetenv("PROTORELAY_NATIVE_POLLER");
    assert(make_poller_from_env(&chosen) && chosen == backend_name(default_poller_backend()));

    // Requesting a backend that is not compiled in yields nullptr.
    const PollerBackend missing = default_poller_backend() == PollerBackend::kqueue
        ? PollerBackend::poll : PollerBackend::kqueue;
    if (!backend_available(missing)) assert(make_poller(missing) == nullptr);
    return 0;
}
