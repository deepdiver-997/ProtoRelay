// pr::fsm engine + Completion primitive tests (design §4 step 0).
//
// Drives every engine decision from fsm-extraction-design §3.2 through the
// zero-IO harness (TestSession + MockConnection): guard violation / missing
// handler replies, the exception barrier, the two sanctioned transition write
// points (sync apply and async Transition::finish from a foreign thread), the
// settle收口 (unpause → state → reply → drain → re-arm read), terminal close,
// the error budget, command matching, strict guards, and the Completion
// exactly-once semantics including the wait-timeout race. BulkAccumulator
// covers the declared-length truncation / overflow push-back skeleton.
#undef NDEBUG
#include <cassert>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include <boost/asio.hpp>

#include "framework/fsm/fsm_completion.h"
#include "framework/fsm/fsm_engine.h"
#include "framework/net/asio_executor.h"
#include "mock_connection.h"
#include "test_session.h"

namespace {

enum class St : int { INIT = 0, READY = 1, BUSY = 2, BYE = 3, COUNT = 4 };
enum class Ev : int { CONNECT = 0, PING = 1, QUIT = 2, COUNT = 3 };

using mail_system::MockConnection;
using mail_system::TestSession;
using Engine = pr::fsm::Engine<MockConnection, St, Ev>;

using SessionPtr = std::shared_ptr<pr::SessionBase<MockConnection>>;

struct Fixture {
    // The session owns the connection (unique_ptr); `conn` is a borrowed view
    // for assertions — copies of MockConnection would carry their own
    // io_context, so writes/posts must land on this exact instance.
    MockConnection* conn = nullptr;
    std::shared_ptr<TestSession> session;
    std::unique_ptr<Engine> engine;

    explicit Fixture(int error_budget = 0, bool strict = false) {
        auto connection = std::make_unique<MockConnection>();
        conn = connection.get();
        conn->set_deferred_read(true); // settle's re-arm parks instead of EOF
        session = std::make_shared<TestSession>(std::move(connection));
        session->set_current_state(static_cast<int>(St::INIT));

        pr::fsm::Desc<St, Ev> desc;
        desc.commands = {{"PING", Ev::PING}, {"quit", Ev::QUIT}};
        desc.guards = {
            {St::INIT, Ev::CONNECT, St::READY},
            {St::READY, Ev::PING, St::READY},
            {St::READY, Ev::QUIT, St::BYE},
            {St::BUSY, Ev::PING, std::nullopt}, // documented self loop
        };
        desc.error_policy = {"-ERR unknown\r\n", error_budget};
        desc.timeout_policy = {"-ERR timeout\r\n", true};
        desc.greeting = "+OK ready\r\n";
        desc.internal_error_reply = "-ERR internal\r\n";
        desc.terminals = {St::BYE};
        desc.state_names = {"INIT", "READY", "BUSY", "BYE"};
        desc.event_names = {"CONNECT", "PING", "QUIT"};

        engine = std::make_unique<Engine>(std::move(desc));
        engine->set_strict_guards(strict);
    }
    ~Fixture() { mail_system::finish_session(session); }

    void pump() { conn->pump_executor(); }
};

int g_pass = 0, g_fail = 0;
void expect_true(bool condition, const char* what) {
    if (condition) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("  FAIL %s\n", what);
    }
}

// ---- engine: banner entry / dispatch semantics ----

void test_start_and_greeting() {
    Fixture f;
    f.engine->start(f.session, St::READY);
    f.pump();
    expect_true(f.conn->written().find("+OK ready") != std::string::npos,
                "start writes the greeting");
    expect_true(f.session->get_current_state() == static_cast<int>(St::READY),
                "start lands in the first state");
    expect_true(!f.session->is_paused(), "start leaves the pipeline running");
}

void test_guard_violation() {
    Fixture f;
    f.engine->add_handler(St::READY, Ev::PING, [](SessionPtr) {}); // unreachable
    f.session->set_current_state(static_cast<int>(St::INIT));
    f.engine->dispatch(f.session, Ev::QUIT); // no (INIT,QUIT) row
    f.pump();
    expect_true(f.conn->written().find("-ERR unknown") != std::string::npos,
                "guard violation writes the error reply");
    expect_true(!f.session->is_closed(), "budget 0 keeps the session open");
}

void test_missing_handler() {
    Fixture f;
    f.session->set_current_state(static_cast<int>(St::READY));
    f.engine->dispatch(f.session, Ev::QUIT); // guard exists, handler does not
    f.pump();
    expect_true(f.conn->written().find("-ERR unknown") != std::string::npos,
                "missing handler writes the fallback reply");
}

void test_exception_barrier() {
    Fixture f;
    f.engine->add_handler(St::READY, Ev::PING, [](SessionPtr) {
        throw std::runtime_error("boom");
    });
    f.engine->add_handler(St::READY, Ev::QUIT, [this_ = f.engine.get()](SessionPtr s) {
        this_->reply(s, "+OK quit\r\n");
    });
    f.session->set_current_state(static_cast<int>(St::READY));
    f.engine->dispatch(f.session, Ev::PING);
    f.pump();
    expect_true(f.conn->written().find("-ERR internal") != std::string::npos,
                "barrier replies the internal error text");
    expect_true(!f.session->is_closed(), "barrier keeps the session open");
    f.engine->dispatch(f.session, Ev::QUIT); // still dispatchable afterwards
    f.pump();
    expect_true(f.conn->written().find("+OK quit") != std::string::npos,
                "session still dispatches after a caught exception");
}

void test_terminal_close() {
    Fixture f;
    f.session->set_current_state(static_cast<int>(St::BYE));
    f.engine->dispatch(f.session, Ev::PING);
    f.pump();
    expect_true(f.session->is_closed(), "terminal state dispatch closes");
}

void test_error_budget() {
    Fixture f(/*error_budget=*/2);
    f.session->set_current_state(static_cast<int>(St::INIT));
    f.engine->dispatch(f.session, Ev::QUIT); // violation #1
    f.pump();
    expect_true(!f.session->is_closed(), "first violation stays inside budget");
    f.engine->dispatch(f.session, Ev::QUIT); // violation #2: budget exhausted
    f.pump();
    expect_true(f.session->is_closed(), "budget exhaustion closes after flush");
}

void test_match_command_and_auto() {
    Fixture f;
    expect_true(f.engine->match_command("ping") == Ev::PING,
                "command matching is case-insensitive");
    expect_true(f.engine->match_command("QUIT") == Ev::QUIT,
                "registered words match regardless of case");
    expect_true(!f.engine->match_command("NOPE").has_value(),
                "unknown words do not match");
    f.engine->add_handler(St::INIT, Ev::CONNECT, [this_ = f.engine.get()](SessionPtr s) {
        this_->reply(s, "+OK connect\r\n");
    });
    f.session->set_next_event(static_cast<int>(Ev::CONNECT));
    f.engine->auto_process_event(f.session);
    f.pump();
    expect_true(f.conn->written().find("+OK connect") != std::string::npos,
                "auto_process_event dispatches the session's next event");
}

// ---- engine: registration-time duplicate detection (silent shadowing) ----

void test_duplicate_handler_registration_throws() {
    pr::fsm::Desc<St, Ev> desc;
    desc.terminals = {St::BYE};
    desc.state_names = {"INIT", "READY", "BUSY", "BYE"};
    desc.event_names = {"CONNECT", "PING", "QUIT"};
    Engine engine(std::move(desc));
    engine.add_handler(St::READY, Ev::PING, [](SessionPtr) {});
    bool threw = false;
    try {
        // One cell, two behaviors: the map assignment would silently shadow
        // the first — registration must fail fast instead.
        engine.add_handler(St::READY, Ev::PING, [](SessionPtr) {});
    } catch (const std::logic_error&) {
        threw = true;
    }
    expect_true(threw, "duplicate handler registration throws logic_error");
}

void test_duplicate_command_word_throws() {
    pr::fsm::Desc<St, Ev> desc;
    desc.commands = {{"PING", Ev::PING}, {"ping", Ev::QUIT}};  // icase duplicate
    desc.state_names = {"INIT", "READY", "BUSY", "BYE"};
    desc.event_names = {"CONNECT", "PING", "QUIT"};
    bool threw = false;
    try {
        Engine engine(std::move(desc));
        (void)engine;
    } catch (const std::logic_error&) {
        threw = true;
    }
    expect_true(threw, "duplicate command word throws at construction");
}

// ---- engine: the two sanctioned write points ----

void test_apply_settles() {
    Fixture f;
    f.session->set_current_state(static_cast<int>(St::READY));
    f.session->append_raw("STAT\n"); // buffered command the收口 must drain
    f.session->set_paused(true);
    f.engine->apply(f.session, St::BUSY, "+OK busy\r\n");
    f.pump();
    expect_true(f.session->get_current_state() == static_cast<int>(St::BUSY),
                "apply writes the target state");
    expect_true(f.conn->written().find("+OK busy") != std::string::npos,
                "apply sends the stashed reply");
    expect_true(!f.session->is_paused(), "apply unpauses");
    expect_true(f.session->buffered_size() == 0, "apply drains buffered commands");
    expect_true(f.session->process_count == 1, "drained command ran once");
    expect_true(f.conn->has_pending_read(), "settle re-arms the read loop");
}

void test_begin_transition_sync_path() {
    Fixture f;
    f.engine->add_handler(St::INIT, Ev::CONNECT, [this_ = f.engine.get()](SessionPtr s) {
        auto transition = this_->begin_transition(s, St::READY, "+OK moved\r\n");
        // Simulated worker completion from a foreign thread.
        std::thread([t = std::move(transition)]() mutable { t.finish(true); }).join();
    });
    f.session->set_paused(false);
    f.engine->dispatch(f.session, Ev::CONNECT);
    expect_true(f.session->is_paused(), "begin_transition pauses immediately");
    f.pump();
    expect_true(f.session->get_current_state() == static_cast<int>(St::READY),
                "finish(true) applies the target state");
    expect_true(f.conn->written().find("+OK moved") != std::string::npos,
                "finish(true) sends the stashed reply");
    expect_true(!f.session->is_paused(), "finish resumes the pipeline");
}

void test_transition_failure_path() {
    Fixture f;
    f.session->set_current_state(static_cast<int>(St::READY));
    auto transition = f.engine->begin_transition(f.session, St::BUSY);
    transition.fail("-ERR busy\r\n");
    f.pump();
    expect_true(f.session->get_current_state() == static_cast<int>(St::READY),
                "fail keeps the state unchanged");
    expect_true(f.conn->written().find("-ERR busy") != std::string::npos,
                "fail sends the error reply");
    expect_true(!f.session->is_paused(), "fail resumes the pipeline");
}

void test_transition_is_one_shot() {
    Fixture f;
    auto transition = f.engine->begin_transition(f.session, St::READY, "+OK a\r\n");
    transition.finish(true);
    f.pump();
    const std::string first = f.conn->written();
    transition.finish(true);  // second completion must be a no-op
    transition.fail("x");     // ditto
    f.pump();
    expect_true(f.conn->written() == first, "transition completes exactly once");
}

void test_strict_guards_accept_table_target() {
    Fixture f(/*error_budget=*/0, /*strict=*/true);
    f.session->set_current_state(static_cast<int>(St::READY));
    // (READY,QUIT)->BYE is a documented row: applying BYE must not abort.
    f.engine->apply(f.session, St::BYE);
    f.pump();
    expect_true(f.session->get_current_state() == static_cast<int>(St::BYE),
                "strict guards accept table-documented targets");
}

// ---- Completion primitive ----

struct CompletionFixture {
    boost::asio::io_context io;
    std::shared_ptr<pr::net::AsioExecutor> executor;
    std::shared_ptr<pr::fsm::Completion> completion;
    std::vector<pr::fsm::Completion::Outcome> outcomes;

    CompletionFixture()
        : executor(std::make_shared<pr::net::AsioExecutor>(io.get_executor())),
          completion(std::make_shared<pr::fsm::Completion>()) {}

    bool drive(std::chrono::milliseconds budget) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            if (io.stopped()) io.restart();
            io.poll();
            if (completion->done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return completion->done();
    }
};

void test_completion_observe_then_finish() {
    CompletionFixture f;
    f.completion->observe(f.executor, [&f](const pr::fsm::Completion::Outcome& o) {
        f.outcomes.push_back(o);
    });
    expect_true(!f.completion->done(), "observe alone does not complete");
    f.completion->finish({true, false});
    expect_true(f.drive(std::chrono::milliseconds(500)), "finish publishes");
    expect_true(f.outcomes.size() == 1 && f.outcomes[0].ok,
                "observer fired once with the ok outcome");
    f.completion->finish({false, true}); // second finish: no-op
    f.drive(std::chrono::milliseconds(50));
    expect_true(f.outcomes.size() == 1, "finish publishes exactly once");
}

void test_completion_wait_timeout() {
    CompletionFixture f;
    f.completion->wait(f.executor, std::chrono::milliseconds(20),
                       [&f](const pr::fsm::Completion::Outcome& o) { f.outcomes.push_back(o); });
    expect_true(f.drive(std::chrono::milliseconds(1000)), "wait timeout completes");
    expect_true(f.outcomes.size() == 1 && f.outcomes[0].timed_out && !f.outcomes[0].ok,
                "timeout outcome is {ok=false, timed_out=true}, once");
    f.completion->finish({true, false}); // raced finish after timeout: dropped
    f.drive(std::chrono::milliseconds(50));
    expect_true(f.outcomes.size() == 1, "late finish after timeout is a no-op");
}

void test_completion_finish_beats_timeout() {
    CompletionFixture f;
    f.completion->wait(f.executor, std::chrono::milliseconds(5000),
                       [&f](const pr::fsm::Completion::Outcome& o) { f.outcomes.push_back(o); });
    f.completion->finish({true, false}); // success wins the race
    expect_true(f.drive(std::chrono::milliseconds(500)), "finish before timeout publishes ok");
    expect_true(f.outcomes.size() == 1 && f.outcomes[0].ok, "success outcome delivered once");
    f.drive(std::chrono::milliseconds(50));
    expect_true(f.outcomes.size() == 1, "canceled timer never fires a second outcome");
}

void test_completion_late_observer_and_cancel_flag() {
    CompletionFixture f;
    f.completion->finish({true, false});
    f.completion->observe(f.executor, [&f](const pr::fsm::Completion::Outcome& o) {
        f.outcomes.push_back(o);
    });
    f.drive(std::chrono::milliseconds(500));
    expect_true(f.outcomes.size() == 1 && f.outcomes[0].ok,
                "late observer fires immediately with the published outcome");
    expect_true(!f.completion->cancel_requested(), "cancel flag starts clear");
    f.completion->request_cancel();
    expect_true(f.completion->cancel_requested(), "cancel flag is visible to producers");
}

// ---- BulkAccumulator ----

void test_bulk_accumulator() {
    pr::fsm::BulkAccumulator bulk(5);
    auto fed = bulk.feed("abc", 3);
    expect_true(fed.result == pr::fsm::BulkAccumulator::Result::need_more && fed.leftover.empty(),
                "partial feed keeps accumulating");
    fed = bulk.feed("deXYZ", 5); // 2 bytes complete, 3 overflow
    expect_true(fed.result == pr::fsm::BulkAccumulator::Result::complete, "length reached completes");
    expect_true(fed.leftover == "XYZ", "overflow bytes are pushed back verbatim");
    expect_true(bulk.data() == "abcde", "truncation at the declared length");
    fed = bulk.feed("more", 4);
    expect_true(fed.result == pr::fsm::BulkAccumulator::Result::need_more && fed.leftover.empty(),
                "feeding a completed accumulator is dropped");

    pr::fsm::BulkAccumulator exact(3);
    const auto boundary = exact.feed("abc", 3);
    expect_true(boundary.result == pr::fsm::BulkAccumulator::Result::complete &&
                boundary.leftover.empty(),
                "exact boundary completes with no leftover");
}

} // namespace

int main() {
    std::printf("fsm_engine_test\n");
    test_start_and_greeting();
    test_guard_violation();
    test_missing_handler();
    test_exception_barrier();
    test_terminal_close();
    test_error_budget();
    test_match_command_and_auto();
    test_duplicate_handler_registration_throws();
    test_duplicate_command_word_throws();
    test_apply_settles();
    test_begin_transition_sync_path();
    test_transition_failure_path();
    test_transition_is_one_shot();
    test_strict_guards_accept_table_target();
    test_completion_observe_then_finish();
    test_completion_wait_timeout();
    test_completion_finish_beats_timeout();
    test_completion_late_observer_and_cancel_flag();
    test_bulk_accumulator();
    std::printf("  pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
