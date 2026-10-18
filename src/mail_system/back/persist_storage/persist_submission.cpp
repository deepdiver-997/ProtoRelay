#include "mail_system/back/persist_storage/persist_submission.h"
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

namespace mail_system::persist_storage {
namespace asio = boost::asio;
bool PersistCompletion::complete(PersistResult result, const SharedPersistStatus& status) {
    if (result.status != PersistStatus::SUCCESS && result.status != PersistStatus::FAILED &&
        result.status != PersistStatus::CANCELLED) return false;
    std::vector<Observer> observers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (result_) return false;
        status.store(result.status);
        result_ = result;
        observers.swap(observers_);
    }
    for (auto& observer : observers) {
        asio::post(observer.executor, [callback = std::move(observer.callback), result] { callback(result); });
    }
    return true;
}
void PersistCompletion::observe(asio::any_io_executor executor, Complete callback) {
    std::optional<PersistResult> ready;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ready = result_;
        if (!ready) { observers_.push_back({executor, std::move(callback)}); return; }
    }
    asio::post(executor, [callback = std::move(callback), result = *ready] { callback(result); });
}
std::optional<PersistResult> PersistCompletion::result() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return result_;
}
bool PersistSubmissionTicket::complete(PersistStatus terminal, std::string error, bool deduplicated) const {
    return completion->complete({mail_id, terminal, std::move(error), deduplicated, false}, status);
}
namespace {
class Wait : public std::enable_shared_from_this<Wait> {
public:
    Wait(PersistSubmissionTicket ticket, asio::any_io_executor executor,
         std::chrono::milliseconds timeout, PersistCompletion::Complete cb)
        : ticket_(std::move(ticket)), executor_(asio::make_strand(executor)), timer_(executor_),
          timeout_(timeout), callback_(std::move(cb)) {}
    void start() {
        auto self = shared_from_this();
        asio::post(executor_, [self] { self->run(); });
    }
private:
    void run() {
        if (!ticket_.valid()) { finish({0, PersistStatus::CANCELLED, "invalid submission ticket"}); return; }
        timer_.expires_after(timeout_);
        auto self = shared_from_this();
        timer_.async_wait([self](auto ec) {
            if (ec || self->done_) return;
            // 已完成但通知还在排队时，优先使用真实终态。
            if (auto result = self->ticket_.completion->result()) self->finish(*result);
            else {
                self->finish({self->ticket_.mail_id, self->ticket_.status.load(), "persistence wait timed out", false, true});
            }
        });
        std::weak_ptr<Wait> weak = self;
        ticket_.completion->observe(executor_, [weak](PersistResult result) {
            if (auto self = weak.lock()) self->finish(std::move(result));
        });
    }
    void finish(PersistResult result) {
        if (done_) return;
        done_ = true; timer_.cancel();
        auto callback = std::move(callback_);
        callback(std::move(result));
    }
    PersistSubmissionTicket ticket_;
    asio::any_io_executor executor_;
    asio::steady_timer timer_;
    std::chrono::milliseconds timeout_;
    PersistCompletion::Complete callback_;
    bool done_ = false;
};
} // namespace
void PersistSubmissionTicket::async_wait(asio::any_io_executor executor, std::chrono::milliseconds timeout,
                                        PersistCompletion::Complete cb) const {
    std::make_shared<Wait>(*this, executor, timeout, std::move(cb))->start();
}
} // namespace mail_system::persist_storage
