// fsm_export.h static topology export tests (docs/local/
// 2026-10-26-fsm-topology-export-brief, Phase 1).
//
// Golden assertions lock the exact DOT and Mermaid rendering on a mini desc
// covering: plain edges, explicit and nullopt self loops, terminal markers,
// orphan states (no guard rows), multi-command events ("/" join) and
// same-key multi-row exits. Real-protocol smoke checks make_pop3_desc /
// make_smtps_desc / make_imaps_desc generate and contain the load-bearing
// nodes/edges. Setting FSM_EXPORT_OUT=<dir> additionally writes the three
// protocols' diagrams there (the regeneration switch behind
// docs/architecture/fsm/ — opt-in, tests never write the repo by default).
#undef NDEBUG
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <functional>
#include <vector>
#include <sstream>
#include <string>
#include <filesystem>

#include "framework/fsm/fsm_export.h"
#include "mail_system/back/mailServer/fsm/pop3/pop3_desc.h"
#include "mail_system/back/mailServer/fsm/smtps/smtps_desc.h"
#include "mail_system/back/mailServer/fsm/imaps/imaps_desc.h"

namespace {

int g_pass = 0, g_fail = 0;
void expect_true(bool condition, const char* what) {
    if (condition) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("  FAIL %s\n", what);
    }
}

enum class St : int { INIT = 0, READY = 1, BUSY = 2, BYE = 3, ORPHAN = 4, COUNT = 5 };
enum class Ev : int { CONNECT = 0, PING = 1, QUIT = 2, ERROR = 3, COUNT = 4 };

pr::fsm::Desc<St, Ev> make_mini_desc() {
    pr::fsm::Desc<St, Ev> d;
    d.commands = {{"PING", Ev::PING}, {"quit", Ev::QUIT}};
    d.guards = {
        {St::INIT, Ev::CONNECT, St::READY},
        {St::READY, Ev::PING, St::READY},   // explicit self loop
        {St::READY, Ev::QUIT, St::BYE},
        {St::BUSY, Ev::ERROR, std::nullopt}, // nullopt self loop
        {St::BUSY, Ev::QUIT, St::READY},     // same-key multi-row: two exits
        {St::BUSY, Ev::QUIT, St::BYE},       // for one (state, event) pair
    };
    d.terminals = {St::BYE};
    d.state_names = {"INIT", "READY", "BUSY", "BYE", "ORPHAN"};
    d.event_names = {"CONNECT", "PING", "QUIT", "ERROR"};
    return d;
}

const char* kGoldenDot =
    "digraph fsm {\n"
    "  rankdir=LR;\n"
    "  node [shape=box fontname=\"monospace\"];\n"
    "  \"INIT\";\n"
    "  \"READY\";\n"
    "  \"BUSY\";\n"
    "  \"BYE\" [peripheries=2];\n"
    "  \"ORPHAN\";\n"
    "  \"INIT\" -> \"READY\" [label=\"CONNECT\"];\n"
    "  \"READY\" -> \"READY\" [label=\"PING {PING}\"];\n"
    "  \"READY\" -> \"BYE\" [label=\"QUIT {quit}\"];\n"
    "  \"BUSY\" -> \"BUSY\" [label=\"ERROR\"];\n"
    "  \"BUSY\" -> \"READY\" [label=\"QUIT {quit}\"];\n"
    "  \"BUSY\" -> \"BYE\" [label=\"QUIT {quit}\"];\n"
    "}\n";

const char* kGoldenMermaid =
    "stateDiagram-v2\n"
    "  note right of BYE : terminal state\n"
    "  INIT --> READY: CONNECT\n"
    "  READY --> READY: PING {PING}\n"
    "  READY --> BYE: QUIT {quit}\n"
    "  BUSY --> BUSY: ERROR\n"
    "  BUSY --> READY: QUIT {quit}\n"
    "  BUSY --> BYE: QUIT {quit}\n";

void test_golden_dot() {
    const auto desc = make_mini_desc();
    std::ostringstream out;
    pr::fsm::write_dot(desc, out);
    expect_true(out.str() == kGoldenDot, "golden DOT output matches exactly");
}

void test_golden_mermaid() {
    const auto desc = make_mini_desc();
    std::ostringstream out;
    pr::fsm::write_mermaid(desc, out);
    expect_true(out.str() == kGoldenMermaid, "golden Mermaid output matches exactly");
}

void test_edge_counts_null_renders_plain() {
    // No counts handed in -> plain rendering (Phase-1 format locked above).
    const auto desc = make_mini_desc();
    std::ostringstream dot, mermaid;
    pr::fsm::write_dot(desc, dot);           // counts omitted -> plain
    pr::fsm::write_mermaid(desc, mermaid);
    expect_true(dot.str() == kGoldenDot, "DOT unchanged without counts");
    expect_true(mermaid.str() == kGoldenMermaid, "Mermaid unchanged without counts");
}

void test_edge_counts_overlay_marks_dead_edges() {
    // Phase 2: an empty (but provided) map means "everything observed to be
    // untaken" — every row annotates ×0 and dead edges get the dashed/red
    // marker in DOT.
    const auto desc = make_mini_desc();
    pr::fsm::EdgeCountMap<St, Ev> counts;   // empty: nothing ever taken
    counts[{St::INIT, Ev::CONNECT, St::READY}] = 1;
    counts[{St::READY, Ev::PING, St::READY}] = 7;
    std::ostringstream dot, mermaid;
    pr::fsm::write_dot(desc, dot, &counts);
    pr::fsm::write_mermaid(desc, mermaid, &counts);
    expect_true(dot.str().find("[label=\"CONNECT x1\"];") != std::string::npos,
                "taken edge shows its count in DOT");
    expect_true(dot.str().find("label=\"PING {PING} x7\"") != std::string::npos,
                "multi-count edge shows its count in DOT");
    expect_true(dot.str().find("label=\"QUIT {quit} x0\", style=dashed, color=red") !=
                    std::string::npos,
                "untaken edges are dashed/red with x0 in DOT");
    expect_true(mermaid.str().find("INIT --> READY: CONNECT x1") != std::string::npos,
                "taken edge shows its count in Mermaid");
    expect_true(mermaid.str().find("QUIT {quit} x0") != std::string::npos,
                "untaken edges show x0 in Mermaid");
}

void test_label_escaping() {
    auto desc = make_mini_desc();
    desc.state_names[0] = "WE\"IRD\\NAME";
    std::ostringstream out;
    pr::fsm::write_dot(desc, out);
    expect_true(out.str().find("\"WE\\\"IRD\\\\NAME\";") != std::string::npos,
                "DOT escapes quotes and backslashes in labels");
}

void test_real_desc_smoke() {
    const auto pop3 = mail_system::make_pop3_desc();
    std::ostringstream pop3_dot;
    pr::fsm::write_dot(pop3, pop3_dot);
    expect_true(pop3_dot.str().find("\"AUTHORIZATION\" -> \"TRANSACTION\" [label=\"PASS {PASS}\"]") !=
                    std::string::npos,
                                "POP3 DOT carries the PASS transition");
    expect_true(pop3_dot.str().find("\"CLOSED\" [peripheries=2]") != std::string::npos,
                "POP3 DOT marks CLOSED terminal");
    expect_true(pop3_dot.str().find("\"INIT\" -> \"INIT\" [label=\"ERROR\"]") !=
                    std::string::npos,
                "POP3 DOT draws the nullopt self loop as a self edge");

    const auto smtps = mail_system::make_smtps_desc(/*allow_starttls=*/true);
    std::ostringstream smtps_dot, smtps_mmd;
    pr::fsm::write_dot(smtps, smtps_dot);
    pr::fsm::write_mermaid(smtps, smtps_mmd);
    // Same-key multi-row exits (AUTH success / failure) all rendered verbatim.
    expect_true(smtps_dot.str().find("\"WAIT_AUTH\" -> \"WAIT_AUTH_USERNAME\" [label=\"AUTH {AUTH}\"]") !=
                    std::string::npos,
                "SMTPS DOT carries the AUTH primary exit");
    expect_true(smtps_dot.str().find("\"WAIT_AUTH\" -> \"WAIT_MAIL_FROM\" [label=\"AUTH {AUTH}\"]") !=
                    std::string::npos,
                "SMTPS DOT carries the AUTH straight-through exit");
    expect_true(smtps_dot.str().find("\"WAIT_AUTH\" -> \"INIT\" [label=\"STARTTLS {STARTTLS}\"]") !=
                    std::string::npos,
                "SMTPS DOT carries STARTTLS when allowed");
    // QUIT lands CLOSED (the 2b-revised real write point), not a self loop.
    expect_true(smtps_mmd.str().find("WAIT_QUIT --> CLOSED: QUIT {QUIT}") != std::string::npos,
                "SMTPS Mermaid carries the QUIT transition to CLOSED");

    const auto imap = mail_system::make_imaps_desc(/*allow_starttls=*/true);
    std::ostringstream imap_dot;
    pr::fsm::write_dot(imap, imap_dot);
    expect_true(imap_dot.str().find("\"LOGOUT\" [peripheries=2]") != std::string::npos,
                "IMAP DOT marks LOGOUT terminal (dual-terminal protocol)");
    expect_true(imap_dot.str().find("\"SELECTED\" -> \"AUTHENTICATED\" [label=\"CLOSE {CLOSE}\"]") !=
                    std::string::npos,
                "IMAP DOT carries the CLOSE transition");
}

void test_generate_diagram_files() {
    // Regeneration switch behind docs/architecture/fsm/ (opt-in via env).
    const char* out_dir = std::getenv("FSM_EXPORT_OUT");
    if (!out_dir || !*out_dir) return;
    std::filesystem::create_directories(out_dir);
    struct Protocol {
        const char* name;
        std::function<void(std::ostream&)> dot;
        std::function<void(std::ostream&)> mermaid;
    };
    std::vector<Protocol> protocols = {
        {"pop3",
         [](std::ostream& o) { pr::fsm::write_dot(mail_system::make_pop3_desc(), o); },
         [](std::ostream& o) { pr::fsm::write_mermaid(mail_system::make_pop3_desc(), o); }},
        {"smtps",
         [](std::ostream& o) { pr::fsm::write_dot(mail_system::make_smtps_desc(true), o); },
         [](std::ostream& o) { pr::fsm::write_mermaid(mail_system::make_smtps_desc(true), o); }},
        {"imap",
         [](std::ostream& o) { pr::fsm::write_dot(mail_system::make_imaps_desc(true), o); },
         [](std::ostream& o) { pr::fsm::write_mermaid(mail_system::make_imaps_desc(true), o); }},
    };
    for (const auto& protocol : protocols) {
        const std::filesystem::path base(out_dir);
        {
            std::ofstream file(base / (std::string(protocol.name) + ".dot"));
            protocol.dot(file);
        }
        {
            std::ofstream file(base / (std::string(protocol.name) + ".mmd"));
            protocol.mermaid(file);
        }
        std::printf("  generated %s.dot / %s.mmd in %s\n",
                    protocol.name, protocol.name, out_dir);
    }
    expect_true(true, "diagram files generated");
}

} // namespace

int main() {
    test_golden_dot();
    test_golden_mermaid();
    test_edge_counts_null_renders_plain();
    test_edge_counts_overlay_marks_dead_edges();
    test_label_escaping();
    test_real_desc_smoke();
    test_generate_diagram_files();
    std::printf("  pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
