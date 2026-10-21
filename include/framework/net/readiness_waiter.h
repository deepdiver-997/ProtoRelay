#pragma once
#include <chrono>
#include <functional>
#include <memory>
#include <system_error>

namespace pr::net {
enum ReadinessEvent : unsigned { ReadyRead = 1, ReadyWrite = 2, ReadyExcept = 4, ReadyTimeout = 8 };
struct ReadinessRequest {
    int native_handle = -1; // borrowed; the waiter must never close the caller's handle
    unsigned events = 0;
    std::chrono::milliseconds timeout{-1}; // negative means no timeout
};
struct ReadinessResult {
    unsigned events = 0;
    std::error_code error;
};
class IReadinessOperation {
public:
    virtual ~IReadinessOperation() = default;
    virtual void cancel() = 0;
};
class IReadinessWaiter {
public:
    using Complete = std::function<void(ReadinessResult)>;
    virtual ~IReadinessWaiter() = default;
    virtual bool asynchronous() const noexcept = 0;
    // Blocking adapters implement wait; callers can iterate without callback recursion.
    virtual ReadinessResult wait(const ReadinessRequest&) {
        return {0, std::make_error_code(std::errc::operation_not_supported)};
    }
    // Async adapters complete once, never inline, on their injected serial executor.
    // Keep that runtime running through completion/cancellation. A returned operation
    // observes a borrowed handle and must not take ownership of the original handle.
    virtual std::shared_ptr<IReadinessOperation> async_wait(ReadinessRequest request, Complete cb) {
        if (cb) cb(wait(request));
        return {};
    }
};
using ReadinessWaiterFactory = std::function<std::shared_ptr<IReadinessWaiter>()>;
std::shared_ptr<IReadinessWaiter> make_poll_readiness_waiter();
}
