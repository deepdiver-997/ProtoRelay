#ifndef PR_FRAMEWORK_NET_EXECUTOR_H
#define PR_FRAMEWORK_NET_EXECUTOR_H
#include <chrono>
#include <functional>
#include <memory>
#include <system_error>

namespace pr::net {
class IExecutor;
class ITimer {
public:
    using Handler = std::function<void(const std::error_code&)>;
    virtual ~ITimer() = default;
    // Like a socket, mutate a timer only on its executor. Rearm/cancel completes
    // displaced waits with operation_canceled, exactly once per wait.
    virtual void expires_after(std::chrono::milliseconds delay) = 0;
    virtual void async_wait(Handler handler) = 0;
    virtual void cancel() = 0;
    virtual std::shared_ptr<IExecutor> executor() const = 0;
};
class IExecutor {
public:
    using Task = std::function<void()>;
    virtual ~IExecutor() = default;
    // Never inline. Tasks and timer callbacks on an executor are serialized.
    // The owning runtime must remain running until all operations complete.
    virtual void post(Task task) = 0;
    virtual std::shared_ptr<ITimer> make_timer() = 0;
};

// IO runtimes provide schedulers independently of their event-loop backend.
// A worker-only ThreadPoolBase need not implement this interface.
class IExecutorProvider {
public:
    virtual ~IExecutorProvider() = default;
    virtual std::shared_ptr<IExecutor> next_executor() = 0;
};
} // namespace pr::net
#endif
