#include "framework/net/asio_readiness_waiter.h"
#include "framework/net/asio_executor.h"
#include <boost/asio/posix/stream_descriptor.hpp>
#include <cerrno>
#include <stdexcept>
#include <unistd.h>

namespace pr::net {
namespace {
class ReadinessOperation final : public IReadinessOperation,
                                 public std::enable_shared_from_this<ReadinessOperation> {
public:
    ReadinessOperation(std::shared_ptr<AsioExecutor> executor, ReadinessRequest request,
                       IReadinessWaiter::Complete complete)
        : executor_(std::move(executor)), request_(request), complete_(std::move(complete)),
          descriptor_(executor_->native_executor()), timer_(executor_->native_executor()) {}
    void start() {
        const unsigned io = request_.events & (ReadyRead | ReadyWrite | ReadyExcept);
        if (!io && request_.timeout.count() < 0) {
            finish({0, std::make_error_code(std::errc::invalid_argument)});
            return;
        }
        auto self = shared_from_this();
        if (io) {
            // Observe a duplicate so executor shutdown, failed setup, cancellation,
            // and operation destruction can never close Connector/C's own socket.
            int duplicate = ::dup(request_.native_handle);
            if (duplicate < 0) { finish({0, std::error_code(errno, std::generic_category())}); return; }
            boost::system::error_code error;
            descriptor_.assign(duplicate, error);
            if (error) { ::close(duplicate); finish({0, asio_error(error)}); return; }
            auto arm = [self](auto kind, unsigned event) {
                self->descriptor_.async_wait(kind, [self, event](const boost::system::error_code& error) {
                    self->finish({error ? 0u : event, asio_error(error)});
                });
            };
            if (io & ReadyRead) arm(boost::asio::posix::stream_descriptor::wait_read, ReadyRead);
            if (io & ReadyWrite) arm(boost::asio::posix::stream_descriptor::wait_write, ReadyWrite);
            if (io & ReadyExcept) arm(boost::asio::posix::stream_descriptor::wait_error, ReadyExcept);
        }
        if (request_.timeout.count() >= 0) {
            timer_.expires_after(request_.timeout);
            timer_.async_wait([self](const boost::system::error_code& error) {
                self->finish({error ? 0u : static_cast<unsigned>(ReadyTimeout), asio_error(error)});
            });
        }
    }
    void cancel() override {
        auto self = shared_from_this();
        executor_->post([self] {
            self->finish({0, std::make_error_code(std::errc::operation_canceled)});
        });
    }
private:
    void finish(ReadinessResult result) {
        if (finished_) return;
        finished_ = true;
        boost::system::error_code ignored;
        descriptor_.cancel(ignored);
        descriptor_.close(ignored);
        try { timer_.cancel(); } catch (const boost::system::system_error&) {}
        auto complete = std::move(complete_);
        if (complete) complete(std::move(result));
    }
    std::shared_ptr<AsioExecutor> executor_;
    ReadinessRequest request_;
    IReadinessWaiter::Complete complete_;
    boost::asio::posix::stream_descriptor descriptor_;
    boost::asio::steady_timer timer_;
    bool finished_ = false;
};
class AsioReadinessWaiter final : public IReadinessWaiter {
public:
    explicit AsioReadinessWaiter(std::shared_ptr<AsioExecutor> executor) : executor_(std::move(executor)) {}
    bool asynchronous() const noexcept override { return true; }
    std::shared_ptr<IReadinessOperation> async_wait(ReadinessRequest request, Complete cb) override {
        auto operation = std::make_shared<ReadinessOperation>(executor_, request, std::move(cb));
        executor_->post([operation] { operation->start(); });
        return operation;
    }
private:
    std::shared_ptr<AsioExecutor> executor_;
};
}
std::shared_ptr<IReadinessWaiter> make_asio_readiness_waiter(std::shared_ptr<IExecutor> executor) {
    auto asio = std::dynamic_pointer_cast<AsioExecutor>(executor);
    if (!asio) throw std::invalid_argument("Asio readiness waiter requires an Asio executor");
    return std::make_shared<AsioReadinessWaiter>(std::move(asio));
}
}
