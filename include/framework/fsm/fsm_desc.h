#ifndef PR_FRAMEWORK_FSM_FSM_DESC_H
#define PR_FRAMEWORK_FSM_FSM_DESC_H
// Pure-data protocol description for the fsm engine
// (docs/local/fsm-extraction-design.md §3.1/§3.2). A description is plain
// struct data filled in user code (axiom 1): no DSL, no config files, no
// codegen. Only std types appear here — a description must be buildable
// without sessions, engines or any framework internals (§3.4 boundary).
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <tuple>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pr::fsm {

// Runtime edge-traversal counts keyed by (from, event, to) — the same
// identity as one guard row (documented self loops normalize to to == from).
// The engine fills it when PR_FSM_EDGE_STATS=1 (atomic counters, engines are
// shared by many sessions/threads); the export overlays it onto the diagram
// so dead edges show up as ×0.
template <typename State, typename Event>
using EdgeCountMap = std::map<std::tuple<State, Event, State>, std::uint64_t>;

template <typename State, typename Event>
struct CommandEntry {
    std::string word; // matched case-insensitively against the first token
    Event event;
};

// Guard row: (from, event) is a legal pairing. `to` documents the intended
// target and is the assertion goal of the engine's strict-guard check
// (nullopt = self loop). The engine NEVER performs the transition itself —
// handlers do, via Engine::apply / Transition::finish, the only sanctioned
// write points (design §3.2 decision 3: today's tables only gate legality;
// the "to" column was never executed by FsmBase).
template <typename State, typename Event>
struct GuardRow {
    State from;
    Event on;
    std::optional<State> to;
};

template <typename State, typename Event>
struct ErrorPolicy {
    std::string reply; // written on guard violation / unknown command; "" = silent
    int budget = 0;    // >0: close the session after this many consecutive errors
};

template <typename State, typename Event>
struct TimeoutPolicy {
    std::string reply;
    bool close = true;
};

template <typename State, typename Event>
struct Desc {
    std::vector<CommandEntry<State, Event>> commands;
    std::vector<GuardRow<State, Event>> guards;
    ErrorPolicy<State, Event> error_policy;
    TimeoutPolicy<State, Event> timeout_policy;
    std::string greeting;             // "" = no banner (HTTP style)
    std::string internal_error_reply; // exception-barrier text; "" = log only
    std::vector<State> terminals;     // dispatch in these states closes
    // Diagnostics tables indexed by enum value (state_names[(size_t)s]).
    std::vector<std::string_view> state_names;
    std::vector<std::string_view> event_names;
    size_t max_line_bytes = 16ull * 1024 * 1024;
    // Stage -> watchdog duration; unset means "use the session default".
    std::function<std::chrono::milliseconds(State)> stage_timeout;
    // Optional reply formatters (e.g. IMAP tagged BAD). Fall back to
    // error_policy.reply when unset or when they return "".
    std::function<std::string(State, Event)> format_violation;
    std::function<std::string(State, Event)> format_missing;

    // Registration-time diagnostic: two entries carrying the same word
    // (case-insensitive) would silently shadow each other in match_command
    // (first match wins). Legal tables have none.
    bool has_conflicting_command() const {
        for (std::size_t i = 0; i < commands.size(); ++i)
            for (std::size_t j = i + 1; j < commands.size(); ++j)
                if (commands[i].word.size() == commands[j].word.size() &&
                    icase_equal(commands[i].word, commands[j].word))
                    return true;
        return false;
    }

    // Case-insensitive first-token lookup. Returns nullopt for unknown words.
    std::optional<Event> match_command(const std::string& word) const {
        if (word.empty()) return std::nullopt;
        for (const auto& entry : commands) {
            if (entry.word.size() == word.size() &&
                icase_equal(entry.word, word))
                return entry.event;
        }
        return std::nullopt;
    }

    bool is_terminal(State s) const {
        for (State t : terminals)
            if (t == s) return true;
        return false;
    }

    // Guard lookup: nullopt = no row (violation); otherwise the row's target
    // (which may itself be nullopt for a documented self loop). Duplicate
    // (from, on) rows are LEGAL multi-exit documentation (SMTP AUTH/DATA_END,
    // HTTP HEADER_END): only presence is consumed by dispatch, strict-guard
    // scans every row, and the export draws all of them. If a consumer ever
    // needs "the" target, note this returns the FIRST matching row.
    std::optional<std::optional<State>> guard_target(State from, Event on) const {
        for (const auto& row : guards) {
            if (row.from == from && row.on == on) return row.to;
        }
        return std::nullopt;
    }

    std::string_view name_of(State s) const {
        const auto index = static_cast<std::size_t>(s);
        return index < state_names.size() ? state_names[index] : "?";
    }
    std::string_view name_of(Event e) const {
        const auto index = static_cast<std::size_t>(e);
        return index < event_names.size() ? event_names[index] : "?";
    }

private:
    static bool icase_equal(const std::string& a, const std::string& b) {
        for (std::size_t i = 0; i < a.size(); ++i) {
            char ca = a[i], cb = b[i];
            if (ca >= 'a' && ca <= 'z') ca = static_cast<char>(ca - 'a' + 'A');
            if (cb >= 'a' && cb <= 'z') cb = static_cast<char>(cb - 'a' + 'A');
            if (ca != cb) return false;
        }
        return true;
    }
};

} // namespace pr::fsm

#endif // PR_FRAMEWORK_FSM_FSM_DESC_H
