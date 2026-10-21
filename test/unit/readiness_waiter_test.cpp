#undef NDEBUG
#include <cassert>
#include "framework/net/asio_readiness_waiter.h"
#include "framework/net/asio_executor.h"
#include <boost/asio/io_context.hpp>
#include <fcntl.h>
#include <csignal>
#include <pthread.h>
#include <thread>
#include <unistd.h>

int main() {
    using namespace pr::net;
    int handles[2];
    assert(::pipe(handles) == 0);
    auto poll = make_poll_readiness_waiter();
    assert(!poll->asynchronous());
    assert(poll->wait({handles[0], ReadyRead, std::chrono::milliseconds(0)}).events == ReadyTimeout);
    assert(::write(handles[1], "a", 1) == 1);
    assert(poll->wait({handles[0], ReadyRead, std::chrono::milliseconds(10)}).events & ReadyRead);
    assert(::fcntl(handles[0], F_GETFD) >= 0);
    assert(poll->wait({-1, ReadyRead, std::chrono::milliseconds(0)}).error);

    boost::asio::io_context io;
    auto executor = std::make_shared<AsioExecutor>(io.get_executor());
    auto waiter = make_asio_readiness_waiter(executor);
    assert(waiter->asynchronous());
    int calls = 0;
    waiter->async_wait({handles[0], ReadyRead, std::chrono::milliseconds(50)}, [&](ReadinessResult result) {
        assert(!result.error && (result.events & ReadyRead));
        assert(::fcntl(handles[0], F_GETFD) >= 0);
        ++calls;
    });
    assert(calls == 0);
    io.run();
    assert(calls == 1);
    char byte;
    assert(::read(handles[0], &byte, 1) == 1);
    // A signal does not make a borrowed descriptor readable.
    struct sigaction previous{}, interrupt{};
    interrupt.sa_handler = +[](int) {};
    sigemptyset(&interrupt.sa_mask);
    assert(::sigaction(SIGUSR1, &interrupt, &previous) == 0);
    const auto owner = ::pthread_self();
    std::thread interrupt_then_write([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        assert(::pthread_kill(owner, SIGUSR1) == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        assert(::write(handles[1], "b", 1) == 1);
    });
    assert(poll->wait({handles[0], ReadyRead, std::chrono::milliseconds(1000)}).events == ReadyRead);
    assert(::read(handles[0], &byte, 1) == 1 && byte == 'b');
    interrupt_then_write.join();
    assert(::sigaction(SIGUSR1, &previous, nullptr) == 0);
    io.restart();
    waiter->async_wait({handles[0], ReadyRead, std::chrono::milliseconds(1)}, [&](ReadinessResult result) {
        assert(!result.error && result.events == ReadyTimeout);
        ++calls;
    });
    io.run();
    assert(calls == 2);
    io.restart();
    auto operation = waiter->async_wait({handles[0], ReadyRead, std::chrono::milliseconds(50)}, [&](ReadinessResult result) {
        assert(result.error == std::errc::operation_canceled);
        ++calls;
    });
    operation->cancel();
    io.run();
    assert(calls == 3 && ::fcntl(handles[0], F_GETFD) >= 0);
    io.restart();
    waiter->async_wait({-1, ReadyRead, std::chrono::milliseconds(1)}, [&](ReadinessResult result) {
        assert(result.error == std::errc::bad_file_descriptor);
        ++calls;
    });
    io.run();
    assert(calls == 4);
    // Runtime destruction discards pending handlers but may close only duplicates.
    {
        boost::asio::io_context stopped;
        auto stopped_executor = std::make_shared<AsioExecutor>(stopped.get_executor());
        auto stopped_waiter = make_asio_readiness_waiter(stopped_executor);
        stopped_waiter->async_wait({handles[0], ReadyRead, std::chrono::milliseconds(-1)},
                                   [](ReadinessResult) { assert(false); });
        assert(stopped.poll_one() == 1); // register the observer, leave the wait pending
    }
    assert(::fcntl(handles[0], F_GETFD) >= 0);
    ::close(handles[0]);
    ::close(handles[1]);
}
