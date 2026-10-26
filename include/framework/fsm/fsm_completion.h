#ifndef PR_FRAMEWORK_FSM_FSM_COMPLETION_H
#define PR_FRAMEWORK_FSM_FSM_COMPLETION_H
// The async command-completion primitive (design §3.2 decision 4, axiom 3):
// a command occupies the session until its (possibly cross-thread) work
// finishes; completion is published exactly once and every notification is
// posted to the caller's executor, so handlers resume serialized on the
// connection thread. Shape lifted from mail_system's PersistCompletion /
// PersistSubmissionTicket (persist_submission.h), which stays untouched and
// independent.
//
// Hold Completion as shared_ptr (wait() arms a timer that keeps itself alive).
// Cancellation only gates work that has not started — it cannot revoke an
// in-flight database transaction (same contract as the persist ticket).
#include "framework/net/executor.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace pr::fsm {

class Completion : public std::enable_shared_from_this<Completion> {
public:
    struct Outcome {
        bool ok = false;
        bool timed_out = false;
    };
    using Callback = std::function<void(Outcome)>;

    // Publish the terminal outcome exactly once; later finish calls are
    // no-ops. Every registered observer is notified on its own executor.
    void finish(Outcome outcome);

    // Subscribe; safe before or after finish (late observers fire immediately
    // on the given executor).
    void observe(std::shared_ptr<net::IExecutor> executor, Callback callback);

    // Subscribe with a deadline: when the timeout fires first, the outcome is
    // {ok=false, timed_out=true}, still published exactly once.
    void wait(std::shared_ptr<net::IExecutor> executor,
              std::chrono::milliseconds timeout, Callback callback);

    // Gates work that has not started yet; consumers poll this flag.
    void request_cancel() { cancel_requested_.store(true, std::memory_order_release); }
    bool cancel_requested() const { return cancel_requested_.load(std::memory_order_acquire); }
    bool done() const;

private:
    struct Observer {
        std::shared_ptr<net::IExecutor> executor;
        Callback callback;
    };
    void arm_timer(std::shared_ptr<net::IExecutor> executor,
                   std::chrono::milliseconds timeout);

    mutable std::mutex mutex_;
    std::optional<Outcome> outcome_;
    std::vector<Observer> observers_;
    std::shared_ptr<net::ITimer> timer_; // armed by wait(); canceled on finish
    std::atomic<bool> cancel_requested_{false};
};

} // namespace pr::fsm

#endif // PR_FRAMEWORK_FSM_FSM_COMPLETION_H
