#ifndef PR_FRAMEWORK_NET_ASIO_EXECUTOR_H
#define PR_FRAMEWORK_NET_ASIO_EXECUTOR_H
// Optional adapter boundary: core interfaces do not include this header.
#include "framework/net/executor.h"
#include "framework/net/asio_error.h"
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

namespace pr::net {
class AsioExecutor final : public IExecutor, public std::enable_shared_from_this<AsioExecutor> {
public:
    explicit AsioExecutor(boost::asio::any_io_executor executor)
        : executor_(asio_call("create executor", [&] {
            return boost::asio::any_io_executor(boost::asio::make_strand(std::move(executor)));
        })) {}
    void post(Task task) override {
        asio_call("post task", [&] { boost::asio::post(executor_, std::move(task)); });
    }
    std::shared_ptr<ITimer> make_timer() override;
    const boost::asio::any_io_executor& native_executor() const { return executor_; }
private:
    boost::asio::any_io_executor executor_;
};
class AsioTimer final : public ITimer {
public:
    explicit AsioTimer(std::shared_ptr<AsioExecutor> executor)
        : executor_(std::move(executor)), timer_(asio_call("create timer", [this] {
            return boost::asio::steady_timer(executor_->native_executor());
        })) {}
    void expires_after(std::chrono::milliseconds delay) override {
        asio_call("set timer expiry", [&] { timer_.expires_after(delay); });
    }
    void async_wait(Handler handler) override {
        asio_call("start timer wait", [&] {
            timer_.async_wait([handler = std::move(handler)](const boost::system::error_code& error) { handler(asio_error(error)); });
        });
    }
    void cancel() override { asio_call("cancel timer", [this] { timer_.cancel(); }); }
    std::shared_ptr<IExecutor> executor() const override { return executor_; }
private:
    std::shared_ptr<AsioExecutor> executor_;
    boost::asio::steady_timer timer_;
};
inline std::shared_ptr<ITimer> AsioExecutor::make_timer() { return std::make_shared<AsioTimer>(shared_from_this()); }
} // namespace pr::net
#endif
