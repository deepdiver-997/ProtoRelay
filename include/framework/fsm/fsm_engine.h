#ifndef PR_FRAMEWORK_FSM_FSM_ENGINE_H
#define PR_FRAMEWORK_FSM_FSM_ENGINE_H
// Protocol FSM engine (design §3.2, six convergence decisions). Semantics
// are the FsmBase superset: terminal close, guard-table legality check,
// handler lookup, pre-dispatch hook — plus the two things FsmBase never had:
// a built-in exception barrier around every handler (generalized from the
// IMAP override) and sanctioned transition write points.
//
//   * The engine NEVER performs a transition from the guard table. Handlers
//     do it via apply() (sync) or a Transition from begin_transition()
//     (async, finish on any thread). Both run the same收口: unpause → set
//     state → optional reply → drain → re-arm the read loop (guarded).
//   * State stays int-erased inside the session (axiom 5); dispatch reads it
//     back through get_current_state().
//   * Handlers keep today's signature (axiom 5): void(SessionPtr).
//
// Lifetime: an Engine is owned by the protocol's FSM object and must outlive
// every Transition it handed out (protocol FSMs are server-lifetime).
// Thread safety: dispatch/apply/start run on the session's IO thread;
// Transition::finish/fail/reply may be called from any thread — they post
// onto the connection executor, serialized with everything else.
#include "framework/fsm/fsm_completion.h"
#include "framework/fsm/fsm_desc.h"
#include "framework/log/log_macros.h"
#include "framework/session_base.h"

#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace pr::fsm {

template <typename ConnectionType, typename State, typename Event>
class Engine {
public:
    using SessionPtr = std::shared_ptr<SessionBase<ConnectionType>>;
    using Handler = std::function<void(SessionPtr)>;

    class Transition {
    public:
        Transition() = default;
        Transition(Transition&&) = default;
        Transition& operator=(Transition&&) = default;

        // Complete the pending step exactly once. ok: the收口 applies the
        // target state (plus the stashed reply) and resumes the pipeline.
        // !ok: the handler has already replied its own error; the pipeline
        // just resumes with the state unchanged.
        void finish(bool ok) {
            if (!engine_ || !session_) return;
            Engine* engine = engine_;
            SessionPtr session = session_;
            auto target = target_;
            auto reply = std::move(reply_);
            release();
            if (session->is_closed()) return;
            post(session, [engine, session, ok, target,
                           reply = std::move(reply)]() mutable {
                if (session->is_closed()) return;
                if (ok) engine->settle(session, target, std::move(reply));
                else engine->settle(session, std::nullopt, std::nullopt);
            });
        }

        // Reply an error line, then resume with the state unchanged.
        void fail(std::string error_reply) {
            if (!engine_ || !session_) return;
            Engine* engine = engine_;
            SessionPtr session = session_;
            release();
            if (session->is_closed()) return;
            post(session, [engine, session,
                           error_reply = std::move(error_reply)]() mutable {
                if (session->is_closed()) return;
                engine->settle(session, std::nullopt, std::move(error_reply));
            });
        }

        // Intermediate write before the eventual finish (any thread).
        void reply(std::string data) {
            if (!session_ || data.empty()) return;
            SessionPtr session = session_;
            post(session, [session, data = std::move(data)]() mutable {
                if (!session->is_closed()) session->do_async_write(data, nullptr);
            });
        }

        bool armed() const { return session_ != nullptr; }

    private:
        friend class Engine;
        Transition(SessionPtr session, Engine* engine, std::optional<State> target,
                   std::optional<std::string> reply)
            : session_(std::move(session)), engine_(engine), target_(target),
              reply_(std::move(reply)) {}

        static void post(const SessionPtr& session, std::function<void()> task) {
            session->get_connection().executor()->post(std::move(task));
        }
        void release() {
            session_.reset();
            engine_ = nullptr;
            target_.reset();
            reply_.reset();
        }

        SessionPtr session_;
        Engine* engine_ = nullptr;
        std::optional<State> target_;
        std::optional<std::string> reply_;
    };

    explicit Engine(Desc<State, Event> desc) : desc_(std::move(desc)) {
        const char* strict = std::getenv("PR_FSM_STRICT_GUARDS");
        strict_guards_ = strict && strict[0] == '1';
    }

    // ---- registration (protocol FSM object, at construction) ----

    void add_handler(State state, Event event, Handler handler) {
        handlers_[{state, event}] = std::move(handler);
    }
    // e.g. IMAP tag storage before dispatch (design §3.4-B).
    void set_event_begin(std::function<void(State, Event, SessionPtr)> hook) {
        event_begin_ = std::move(hook);
    }
    // Diagnostic/log hook before a handler runs.
    void set_pre_dispatch(std::function<void(State, Event, SessionPtr)> hook) {
        pre_dispatch_ = std::move(hook);
    }
    // Guard-table drift check (design §3.2-3): apply/finish abort when the
    // written target is not the `to` of any guard row leaving the current
    // state. Off by default; PR_FSM_STRICT_GUARDS=1 flips the default.
    void set_strict_guards(bool enabled) { strict_guards_ = enabled; }

    // ---- entry points ----

    // Banner entry: writes desc.greeting (when set) and lands the session in
    // first_state with the pipeline resumed — the SMTP/IMAP write-callback
    // banner shape generalized.
    void start(const SessionPtr& session, State first_state) {
        if (!session || session->is_closed()) return;
        if (!desc_.greeting.empty())
            session->do_async_write(desc_.greeting, nullptr);
        settle(session, first_state, std::nullopt);
    }

    void dispatch(const SessionPtr& session, Event event) {
        if (!session || session->is_closed()) return;
        const State current = static_cast<State>(session->get_current_state());
        if (desc_.is_terminal(current)) {
            session->close();
            return;
        }
        if (!desc_.guard_target(current, event).has_value()) {
            violate(session, current, event);
            return;
        }
        const auto handler = handlers_.find({current, event});
        if (handler == handlers_.end()) {
            write_reply(session, missing_reply(current, event));
            bump_error_budget(session);
            return;
        }
        if (event_begin_) event_begin_(current, event, session);
        if (pre_dispatch_) pre_dispatch_(current, event, session);
        try {
            handler->second(session);
        } catch (const std::exception& error) {
            // Barrier (design §3.2-1): a throwing handler must not pierce the
            // IO thread run loop (smtps fsm tpp header note); it costs one
            // protocol error reply instead.
            LOG_SESSION_ERROR("FSM handler threw (state={} event={}): {}",
                              std::string(desc_.name_of(current)),
                              std::string(desc_.name_of(event)), error.what());
            write_reply(session, desc_.internal_error_reply);
        } catch (...) {
            LOG_SESSION_ERROR("FSM handler threw a non-standard exception (state={} event={})",
                              std::string(desc_.name_of(current)),
                              std::string(desc_.name_of(event)));
            write_reply(session, desc_.internal_error_reply);
        }
    }

    // The session-driven entry: parse filled next_event_, dispatch it.
    void auto_process_event(const SessionPtr& session) {
        if (!session) return;
        dispatch(session, static_cast<Event>(session->get_next_event()));
    }

    // ---- sanctioned transition write points ----

    // Sync: the收口 (unpause → state → reply → drain → re-arm read) right here.
    void apply(const SessionPtr& session, State target,
               std::optional<std::string> reply = std::nullopt) {
        settle(session, std::optional<State>(target), std::move(reply));
    }

    // Async: pause takes effect immediately (the IO pipeline stops consuming
    // while the work runs); finish()/fail() may come from any thread.
    [[nodiscard]] Transition begin_transition(const SessionPtr& session,
                                               std::optional<State> target,
                                               std::optional<std::string> reply = std::nullopt) {
        if (session) session->set_paused(true);
        return Transition(session, this, target, std::move(reply));
    }

    // One queued write; safe from any thread (do_async_write posts).
    void reply(const SessionPtr& session, std::string data) {
        if (!session || data.empty()) return;
        session->do_async_write(data, nullptr);
    }

    const Desc<State, Event>& desc() const { return desc_; }
    std::optional<Event> match_command(const std::string& word) const {
        return desc_.match_command(word);
    }

private:
    friend class Transition;

    // The single收口 replacing today's per-handler set_state + drain +
    // do_async_read triples (30 set / 15 drain in SMTP alone).
    void settle(const SessionPtr& session, std::optional<State> target,
                std::optional<std::string> reply) {
        if (!session || session->is_closed()) return;
        if (target) check_strict_guard(session, *target);
        session->set_paused(false);
        if (target) session->set_current_state(static_cast<int>(*target));
        if (reply && !reply->empty()) session->do_async_write(*reply, nullptr);
        session->drain_buffered_commands();
        // drain never arms new reads (its own header note); re-arm when the
        // pipeline is idle. do_async_read's own guards make this a no-op
        // when buffered input remains or a read is already in flight.
        if (!session->is_paused()) session->do_async_read();
    }

    void violate(const SessionPtr& session, State state, Event event) {
        write_reply(session, violation_reply(state, event));
        bump_error_budget(session);
    }

    void write_reply(const SessionPtr& session, const std::string& text) {
        if (session && !session->is_closed() && !text.empty())
            session->do_async_write(text, nullptr);
    }

    std::string violation_reply(State state, Event event) const {
        if (desc_.format_violation) {
            std::string formatted = desc_.format_violation(state, event);
            if (!formatted.empty()) return formatted;
        }
        return desc_.error_policy.reply;
    }
    std::string missing_reply(State state, Event event) const {
        if (desc_.format_missing) {
            std::string formatted = desc_.format_missing(state, event);
            if (!formatted.empty()) return formatted;
        }
        return desc_.error_policy.reply;
    }

    void bump_error_budget(const SessionPtr& session) {
        if (!session || desc_.error_policy.budget <= 0) return;
        if (++session->stay_times_ < desc_.error_policy.budget) return;
        // Budget exhausted: the reply above is already queued; let it flush
        // before the socket goes away.
        session->close_after_flush();
    }

    void check_strict_guard(const SessionPtr& session, State target) {
        if (!strict_guards_) return;
        const State from = static_cast<State>(session->get_current_state());
        if (from == target) return;
        for (const auto& row : desc_.guards) {
            if (row.from == from && row.to && *row.to == target) return;
        }
        LOG_SESSION_ERROR("strict guards: {} -> {} has no guard row (table drift)",
                          std::string(desc_.name_of(from)), std::string(desc_.name_of(target)));
        std::abort();
    }

    Desc<State, Event> desc_;
    std::map<std::pair<State, Event>, Handler> handlers_;
    std::function<void(State, Event, SessionPtr)> event_begin_;
    std::function<void(State, Event, SessionPtr)> pre_dispatch_;
    bool strict_guards_ = false;
};

// Bulk-data skeleton (design §3.2-5): declared-length delimination with
// at-length truncation and overflow push-back, lifted from the IMAP literal
// state machine (imaps_session.tpp). Pure logic — sessions wire it to their
// read path; the engine never parses.
class BulkAccumulator {
public:
    explicit BulkAccumulator(std::size_t expected) : expected_(expected) {}

    enum class Result { need_more, complete };

    struct Feed {
        Result result = Result::need_more;
        std::string leftover; // bytes past the declared length, pushed back
    };

    Feed feed(const char* data, std::size_t size) {
        Feed out;
        if (done_ || size == 0) return out;
        const std::size_t room = expected_ - sink_.size();
        const std::size_t take = size < room ? size : room;
        sink_.append(data, take);
        if (sink_.size() == expected_) {
            done_ = true;
            out.result = Result::complete;
            out.leftover.assign(data + take, size - take);
        }
        return out;
    }

    const std::string& data() const { return sink_; }
    bool complete() const { return done_; }

private:
    std::size_t expected_;
    std::string sink_;
    bool done_ = false;
};

} // namespace pr::fsm

#endif // PR_FRAMEWORK_FSM_FSM_ENGINE_H
