#ifndef PR_FRAMEWORK_FSM_FSM_EXPORT_H
#define PR_FRAMEWORK_FSM_FSM_EXPORT_H
// Static topology export for protocol FSM descriptions (docs/local/
// 2026-10-26-fsm-topology-export-brief, Phase 1). Pure functions over
// fsm::Desc — std-only, zero mail_system includes (same boundary as
// fsm_desc.h). These render what the tables DECLARE: every guard row is
// drawn, same-key multi-row exits (SMTP AUTH's multi-exit) all appear, and
// no runtime path is guessed. States not referenced by any guard row still
// get nodes (orphan states are information too).
//
// Phase 2 (runtime edge counts): pass Engine::edge_count_snapshot() as
// `edge_counts`. Each guard row is annotated with its observed traversal
// count — "×0" rows (dashed/red in DOT) are dead edges, i.e. declared
// transitions no executed path has ever taken.
#include "framework/fsm/fsm_desc.h"

#include <optional>
#include <ostream>
#include <string>

namespace pr::fsm {

namespace detail {

// DOT/Mermaid-safe escaping for labels (quotes and backslashes).
inline std::string escape_label(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

// Commands that trigger `event`, joined with "/" (e.g. "EHLO/HELO"), or ""
// when the protocol parses events without a command word (HTTP).
template <typename State, typename Event>
std::string command_words_for(const Desc<State, Event>& desc, Event event) {
    std::string joined;
    for (const auto& entry : desc.commands) {
        if (entry.event != event) continue;
        if (!joined.empty()) joined += "/";
        joined += entry.word;
    }
    return joined;
}

} // namespace detail

// Graphviz DOT. Terminals get a double border (peripheries=2).
template <typename State, typename Event>
void write_dot(const Desc<State, Event>& desc, std::ostream& out,
               const EdgeCountMap<State, Event>* edge_counts = nullptr) {
    auto count_of = [&](const GuardRow<State, Event>& row)
        -> std::optional<std::uint64_t> {
        if (!edge_counts) return std::nullopt;
        auto it = edge_counts->find({row.from, row.on, row.to.value_or(row.from)});
        return it == edge_counts->end() ? std::optional<std::uint64_t>(0)
                                        : std::optional<std::uint64_t>(it->second);
    };
    out << "digraph fsm {\n";
    out << "  rankdir=LR;\n";
    out << "  node [shape=box fontname=\"monospace\"];\n";
    // Every enum state gets a node, terminal or not, referenced or not.
    for (int i = 0; i < static_cast<int>(State::COUNT); ++i) {
        const auto s = static_cast<State>(i);
        out << "  \"" << detail::escape_label(std::string(desc.name_of(s))) << "\"";
        if (desc.is_terminal(s)) out << " [peripheries=2]";
        out << ";\n";
    }
    for (const auto& row : desc.guards) {
        const auto to = row.to.value_or(row.from); // nullopt = self loop
        std::string label = std::string(desc.name_of(row.on));
        std::string words = detail::command_words_for(desc, row.on);
        if (!words.empty()) label += " {" + words + "}";
        const auto count = count_of(row);
        if (count) label += " x" + std::to_string(*count);
        out << "  \"" << detail::escape_label(std::string(desc.name_of(row.from)))
            << "\" -> \"" << detail::escape_label(std::string(desc.name_of(to)))
            << "\" [label=\"" << detail::escape_label(label) << "\"";
        if (count && *count == 0) out << ", style=dashed, color=red";
        out << "];\n";
    }
    out << "}\n";
}

// Mermaid stateDiagram-v2 (renders directly on GitHub / in docs).
template <typename State, typename Event>
void write_mermaid(const Desc<State, Event>& desc, std::ostream& out,
                   const EdgeCountMap<State, Event>* edge_counts = nullptr) {
    auto count_of = [&](const GuardRow<State, Event>& row)
        -> std::optional<std::uint64_t> {
        if (!edge_counts) return std::nullopt;
        auto it = edge_counts->find({row.from, row.on, row.to.value_or(row.from)});
        return it == edge_counts->end() ? std::optional<std::uint64_t>(0)
                                        : std::optional<std::uint64_t>(it->second);
    };
    out << "stateDiagram-v2\n";
    // Terminal marker: Mermaid v2 has no double-border primitive; a note per
    // terminal keeps the information visible in every renderer.
    for (State terminal : desc.terminals) {
        out << "  note right of " << detail::escape_label(std::string(desc.name_of(terminal)))
            << " : terminal state\n";
    }
    for (const auto& row : desc.guards) {
        const auto to = row.to.value_or(row.from); // nullopt = self loop
        std::string label = std::string(desc.name_of(row.on));
        std::string words = detail::command_words_for(desc, row.on);
        if (!words.empty()) label += " {" + words + "}";
        const auto count = count_of(row);
        if (count) label += " x" + std::to_string(*count);
        out << "  " << detail::escape_label(std::string(desc.name_of(row.from)))
            << " --> " << detail::escape_label(std::string(desc.name_of(to)))
            << ": " << detail::escape_label(label) << "\n";
    }
}

} // namespace pr::fsm

#endif // PR_FRAMEWORK_FSM_FSM_EXPORT_H
