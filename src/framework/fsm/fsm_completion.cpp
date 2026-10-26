// Compiled half of the fsm Completion primitive (see fsm_completion.h).
// Part of the framework static library; the engine template itself stays in
// the header (protocol State/Event enums are an open set — the explicit
// instantiation suggested by the design is impossible for them, so the
// engine follows the session_base.h/.tpp header precedent instead).
#include "framework/fsm/fsm_completion.h"

namespace pr::fsm {

void Completion::finish(Outcome outcome) {
    std::vector<Observer> observers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (outcome_.has_value()) return; // exactly-once terminal publication
        outcome_ = outcome;
        observers = std::move(observers_);
        observers_.clear();
    }
    if (timer_) {
        // The timer belongs to its executor; route the cancel there so the
        // pending wait is always released (and its captured self dropped).
        if (const auto executor = timer_->executor()) {
            auto timer = timer_;
            executor->post([timer] { timer->cancel(); });
        } else {
            timer_->cancel();
        }
        timer_.reset();
    }
    for (auto& observer : observers) {
        if (!observer.executor || !observer.callback) continue;
        auto executor = std::move(observer.executor);
        auto callback = std::move(observer.callback);
        executor->post([callback = std::move(callback), outcome] { callback(outcome); });
    }
}

void Completion::observe(std::shared_ptr<net::IExecutor> executor, Callback callback) {
    std::optional<Outcome> published;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (outcome_.has_value()) {
            published = *outcome_;
        } else {
            observers_.push_back({std::move(executor), std::move(callback)});
            return;
        }
    }
    if (executor && callback) {
        const Outcome snapshot = *published;
        executor->post([callback = std::move(callback), snapshot] { callback(snapshot); });
    }
}

void Completion::wait(std::shared_ptr<net::IExecutor> executor,
                      std::chrono::milliseconds timeout, Callback callback) {
    observe(executor, std::move(callback));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (outcome_.has_value() || timer_) return; // done already, or armed
    }
    arm_timer(std::move(executor), timeout);
}

void Completion::arm_timer(std::shared_ptr<net::IExecutor> executor,
                           std::chrono::milliseconds timeout) {
    if (!executor) return;
    auto self = shared_from_this();
    // Arm on the timer's own executor (mutation belongs there); the wait
    // handler only publishes on a real expiry — a cancel surfaces as an
    // error code and is ignored.
    executor->post([self, executor, timeout] {
        auto timer = executor->make_timer();
        timer->expires_after(timeout);
        timer->async_wait([self](const std::error_code& error) {
            if (!error) self->finish(Outcome{false, true});
        });
        std::lock_guard<std::mutex> lock(self->mutex_);
        if (self->outcome_.has_value()) {
            // finish() raced the arming: release the just-armed wait.
            auto to_cancel = std::move(timer);
            if (to_cancel) {
                // still on the timer's executor thread here
                to_cancel->cancel();
            }
            return;
        }
        self->timer_ = std::move(timer);
    });
}

bool Completion::done() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return outcome_.has_value();
}

} // namespace pr::fsm
