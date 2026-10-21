#include "framework/net/readiness_waiter.h"
#include <cerrno>
#include <climits>
#include <poll.h>

namespace pr::net {
namespace {
class PollReadinessWaiter final : public IReadinessWaiter {
public:
    bool asynchronous() const noexcept override { return false; }
    ReadinessResult wait(const ReadinessRequest& request) override {
        const unsigned io = request.events & (ReadyRead | ReadyWrite | ReadyExcept);
        if (io && request.native_handle < 0)
            return {0, std::make_error_code(std::errc::bad_file_descriptor)};
        if (!io && request.timeout.count() < 0)
            return {0, std::make_error_code(std::errc::invalid_argument)};
        pollfd descriptor{request.native_handle, 0, 0};
        if (io & ReadyRead) descriptor.events |= POLLIN;
        if (io & ReadyWrite) descriptor.events |= POLLOUT;
        if (io & ReadyExcept) descriptor.events |= POLLPRI;
        const auto timeout = request.timeout.count();
        const int milliseconds = timeout < 0 ? -1 : static_cast<int>(timeout > INT_MAX ? INT_MAX : timeout);
        const int count = ::poll(io ? &descriptor : nullptr, io ? 1 : 0, milliseconds);
        if (count < 0) {
            if (errno == EINTR) return {request.events, {}};
            return {0, std::error_code(errno, std::generic_category())};
        }
        if (count == 0) return {ReadyTimeout, {}};
        unsigned ready = 0;
        if (descriptor.revents & POLLIN) ready |= ReadyRead;
        if (descriptor.revents & POLLOUT) ready |= ReadyWrite;
        if (descriptor.revents & POLLPRI) ready |= ReadyExcept;
        if (descriptor.revents & POLLNVAL)
            return {0, std::make_error_code(std::errc::bad_file_descriptor)};
        // Drain readable bytes even when a peer also closed its end of the stream.
        if (!ready && (descriptor.revents & (POLLERR | POLLHUP)))
            return {0, std::make_error_code(std::errc::io_error)};
        return {ready ? ready : request.events, {}};
    }
};
}
std::shared_ptr<IReadinessWaiter> make_poll_readiness_waiter() {
    return std::make_shared<PollReadinessWaiter>();
}
}
