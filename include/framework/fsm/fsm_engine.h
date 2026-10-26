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

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <ostream>
#include <stdexcept>
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

    // Which fallback chain a reply_formatter entry overrides. The desc's
    // formatters are pure functions of (state, event) — they cannot reach
    // per-session data (IMAP's tag lives in the context); a protocol FSM
    // registers this session-aware hook instead. Return "" to fall through
    // to the desc chain (format_violation/format_missing → error_policy /
    // internal_error_reply).
    enum class ReplyKind { violation, missing, internal_error };
    using ReplyFormatter =
        std::function<std::string(ReplyKind, const SessionPtr&, State, Event)>;

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

        // Complete with an explicit outcome target/reply: for async handlers
        // whose exit state depends on the result (SMTP AUTH failure backs up
        // to WAIT_AUTH while success advances to WAIT_MAIL_FROM — a single
        // stashed target cannot express both). Same收口 as finish(ok), any
        // thread.
        void finish(State target, std::optional<std::string> reply = std::nullopt) {
            if (!engine_ || !session_) return;
            Engine* engine = engine_;
            SessionPtr session = session_;
            release();
            if (session->is_closed()) return;
            post(session, [engine, session, target,
                           reply = std::move(reply)]() mutable {
                if (session->is_closed()) return;
                engine->settle(session, std::optional<State>(target), std::move(reply));
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

        // Closing variant: applies the target state, but the pipeline is NOT
        // resumed — no drain, no read re-arm. For terminal steps (QUIT /
        // LOGOUT family) where buffered input must die with the connection,
        // exactly like today's handlers.
        void finish_closing() {
            if (!engine_ || !session_) return;
            Engine* engine = engine_;
            SessionPtr session = session_;
            auto target = target_;
            release();
            if (session->is_closed()) return;
            post(session, [engine, session, target]() {
                if (session->is_closed()) return;
                engine->settle_closing(session, target);
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
        if (desc_.has_conflicting_command()) {
            throw std::logic_error(
                "fsm desc: duplicate command word (match_command would shadow)");
        }
    }

    // ---- registration (protocol FSM object, at construction) ----

    void add_handler(State state, Event event, Handler handler) {
        // Registration overlap is always a bug: one cell, two behaviors — the
        // map assignment would silently shadow the first (the same hazard the
        // retired array engine had in add_transition). Fail at registration —
        // protocol FSMs register at startup, so this is fail-fast.
        const auto [it, inserted] = handlers_.try_emplace(
            std::pair<State, Event>{state, event}, std::move(handler));
        if (!inserted) {
            throw std::logic_error(
                "fsm duplicate handler registration: " +
                std::string(desc_.name_of(state)) + "/" +
                std::string(desc_.name_of(event)));
        }
    }
    // e.g. IMAP tag storage before dispatch (design §3.4-B).
    void set_event_begin(std::function<void(State, Event, SessionPtr)> hook) {
        event_begin_ = std::move(hook);
    }
    // Diagnostic/log hook before a handler runs.
    void set_pre_dispatch(std::function<void(State, Event, SessionPtr)> hook) {
        pre_dispatch_ = std::move(hook);
    }
    // Side effect to run when a guard violation is replied (e.g. SMTP's
    // session->set_error(InvalidCommand) alongside the 500). The desc's reply
    // formatters are pure functions of (state, event); anything needing the
    // session goes here.
    void set_violation_observer(std::function<void(SessionPtr)> hook) {
        violation_observer_ = std::move(hook);
    }
    // Session-aware reply override (e.g. IMAP's tag echo on BAD/NO). See
    // ReplyKind for the fallback chain.
    void set_reply_formatter(ReplyFormatter hook) {
        reply_formatter_ = std::move(hook);
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
            // Side-effect observer fires here too: a rejection is a rejection,
            // whether the guard table or the handler table rejected it (HTTP's
            // retired fallback handlers both landed in its 400-and-close).
            if (violation_observer_) violation_observer_(session);
            write_reply(session, missing_reply(current, event, session));
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
            write_reply(session, internal_error_reply(current, event, session));
        } catch (...) {
            LOG_SESSION_ERROR("FSM handler threw a non-standard exception (state={} event={})",
                              std::string(desc_.name_of(current)),
                              std::string(desc_.name_of(event)));
            write_reply(session, internal_error_reply(current, event, session));
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

    // Sync closing variant of apply: unpause + state, nothing else — the
    // caller replies and closes. Buffered input dies with the connection,
    // matching today's QUIT/LOGOUT handlers (no drain on closing paths).
    void apply_closing(const SessionPtr& session, State target) {
        settle_closing(session, std::optional<State>(target));
    }

    // Shape-A收口 (banner / prompts / policy rejections): reply, land the
    // target state and re-arm the read — but do NOT drain buffered commands.
    // This is today's "write callback: ec→log+return, else set_state+read"
    // family verbatim (SMTP greeting/EHLO/334/5xx and POP3-style rejects);
    // the settle() drain is the strictly safer superset used elsewhere.
    void reply_then_settle(const SessionPtr& session, std::string text, State target) {
        if (!session || session->is_closed()) return;
        check_strict_guard(session, target);
        write_reply(session, text);
        session->set_paused(false);
        session->set_current_state(static_cast<int>(target));
        session->do_async_read();
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

    // Registration audit (code-review companion): one deterministic line per
    // finding. MULTI-EXIT keys are legal — async outcomes choose the exit at
    // completion time — but every declared exit must have a writer. DUPLICATE
    // rows are pure redundancy. NO-GUARD handlers are unreachable (dispatch
    // checks the guard table first and rejects). NO-HANDLER rows fall to the
    // missing/rejection reply path (sometimes intentional: defensive
    // legality rows).
    void write_registration_audit(std::ostream& out) const {
        std::map<std::pair<State, Event>, std::vector<std::optional<State>>> by_key;
        for (const auto& row : desc_.guards)
            by_key[{row.from, row.on}].push_back(row.to);

        for (const auto& [key, targets] : by_key) {
            const auto& [state, event] = key;
            const bool handler = handlers_.count(key) != 0;
            std::vector<std::string> distinct;   // first-seen order
            for (const auto& t : targets) {
                std::string n = t ? std::string(desc_.name_of(*t)) : "(self)";
                if (std::find(distinct.begin(), distinct.end(), n) == distinct.end())
                    distinct.push_back(std::move(n));
            }
            if (targets.size() >= 2) {
                if (distinct.size() == 1) {
                    out << "DUPLICATE   " << desc_.name_of(state) << " + "
                        << desc_.name_of(event) << ": " << targets.size()
                        << " identical rows (" << distinct.front()
                        << "); handler: " << (handler ? "yes" : "no") << "\n";
                } else {
                    out << "MULTI-EXIT  " << desc_.name_of(state) << " + "
                        << desc_.name_of(event) << ": " << distinct.size()
                        << " exits from " << targets.size() << " rows (";
                    for (std::size_t i = 0; i < distinct.size(); ++i)
                        out << (i ? ", " : "") << distinct[i];
                    out << "); handler: " << (handler ? "yes" : "no") << "\n";
                }
            } else if (!handler) {
                out << "NO-HANDLER  " << desc_.name_of(state) << " + "
                    << desc_.name_of(event) << ": guard row without handler"
                    << " (dispatch falls to the missing/rejection reply)\n";
            }
        }
        for (const auto& [key, handler] : handlers_) {
            (void)handler;
            if (!desc_.guard_target(key.first, key.second).has_value())
                out << "NO-GUARD    " << desc_.name_of(key.first) << " + "
                    << desc_.name_of(key.second)
                    << ": handler without guard row"
                    << " (unreachable — dispatch rejects first)\n";
        }
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
        if (reply && !reply->empty()) write_reply(session, *reply);
        session->drain_buffered_commands();
        // drain never arms new reads (its own header note); re-arm when the
        // pipeline is idle. do_async_read's own guards make this a no-op
        // when buffered input remains or a read is already in flight.
        if (!session->is_paused()) session->do_async_read();
    }

    void violate(const SessionPtr& session, State state, Event event) {
        if (violation_observer_) violation_observer_(session);
        write_reply(session, violation_reply(state, event, session));
        bump_error_budget(session);
    }

    // Closing-path收口: strict check + unpause + state; no drain, no re-arm.
    void settle_closing(const SessionPtr& session, std::optional<State> target) {
        if (!session || session->is_closed()) return;
        if (target) check_strict_guard(session, *target);
        session->set_paused(false);
        if (target) session->set_current_state(static_cast<int>(*target));
    }

    void write_reply(const SessionPtr& session, const std::string& text) {
        if (!session || session->is_closed() || text.empty()) return;
        // Same wire shape as the protocols' send_line helpers: complete the
        // line ending and record the outbound trace before queueing.
        std::string out = text;
        if (out.empty() || out.back() != '\n') out += "\r\n";
        session->trace_append_outbound(out);
        session->do_async_write(out, nullptr);
    }

    std::string violation_reply(State state, Event event, const SessionPtr& session) const {
        if (reply_formatter_) {
            std::string formatted =
                reply_formatter_(ReplyKind::violation, session, state, event);
            if (!formatted.empty()) return formatted;
        }
        if (desc_.format_violation) {
            std::string formatted = desc_.format_violation(state, event);
            if (!formatted.empty()) return formatted;
        }
        return desc_.error_policy.reply;
    }
    std::string missing_reply(State state, Event event, const SessionPtr& session) const {
        if (reply_formatter_) {
            std::string formatted =
                reply_formatter_(ReplyKind::missing, session, state, event);
            if (!formatted.empty()) return formatted;
        }
        if (desc_.format_missing) {
            std::string formatted = desc_.format_missing(state, event);
            if (!formatted.empty()) return formatted;
        }
        return desc_.error_policy.reply;
    }
    std::string internal_error_reply(State state, Event event,
                                     const SessionPtr& session) const {
        if (reply_formatter_) {
            std::string formatted =
                reply_formatter_(ReplyKind::internal_error, session, state, event);
            if (!formatted.empty()) return formatted;
        }
        return desc_.internal_error_reply;
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
    std::function<void(SessionPtr)> violation_observer_;
    ReplyFormatter reply_formatter_;
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
