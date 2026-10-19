// 2026-10-19: POP3 timers bypassed the runtime interface; DB completion rearmed
// a timer on a worker and shutdown could race a ready sweeper callback.
// Regression contract: docs/style/PROJECT_STYLE.md section 11.1.
// Manual timers assert executor affinity, so a direct worker rearm/cancel
// fails deterministically; no sleeps, live database, or real expiry required.
#undef NDEBUG
#include "manual_executor.h"
#include "mock_connection.h"
#include "mock_db_pool.h"
#include "mail_system/back/mailServer/pop3_server.h"
#include "mail_system/back/mailServer/fsm/pop3/traditional_pop3_fsm.tpp"
#include "mail_system/back/mailServer/session/pop3_session.tpp"
#include "framework/session_base.tpp"
#include "mail_system/back/router/static_shard_router.h"
#include <cassert>

namespace {
using mail_system::test::ManualExecutor;
class QueuedPool : public pr::ThreadPoolBase {
public:
    void start() override { running = true; }
    void stop(bool = true) override { running = false; }
    bool is_running() const override { return running; }
    size_t thread_count() const override { return 1; }
    void drain() { while (!tasks.empty()) { auto task = std::move(tasks.front()); tasks.pop_front(); task(); } }
    bool running = true;
    std::deque<std::function<void()>> tasks;
protected:
    void post_impl(std::function<void()> task) override { assert(running); tasks.push_back(std::move(task)); }
};
class ManualIoRuntime final : public QueuedPool, public pr::net::IExecutorProvider {
public:
    std::shared_ptr<pr::net::IExecutor> next_executor() override { return executor; }
    std::shared_ptr<ManualExecutor> executor = std::make_shared<ManualExecutor>();
};
using Router = mail_system::router::StaticShardRouter;
std::shared_ptr<Router> make_router(std::shared_ptr<pr::DBPool> database = {}) {
    return std::make_shared<Router>(std::vector<std::pair<std::string, int>>{}, 0,
        std::vector<std::shared_ptr<pr::DBPool>>{std::move(database)},
        std::vector<std::shared_ptr<pr::storage::IStorageProvider>>{});
}
void test_sweeper_lifetime() {
    auto runtime = std::make_shared<ManualIoRuntime>();
    auto worker = std::make_shared<QueuedPool>();
    auto router = make_router();
    mail_system::ServerConfig config;
    config.address = "127.0.0.1";
    config.listeners.clear(); // No listeners: test only maintenance scheduling.
    config.metrics_enabled = false;
    auto make_server = [&] {
        return std::make_unique<mail_system::Pop3Server>(config, runtime, worker, router);
    };
    {
        auto server = make_server(); server->start();
        auto timer = runtime->executor->timers.back();
        assert(timer->waits == 0);
        server.reset(); // stop before the queued initialization is executed
        runtime->executor->drain();
        assert(timer->waits == 0 && timer->cancels == 1 && worker->tasks.empty());
    }
    {
        auto server = make_server(); server->start(); runtime->executor->drain();
        auto timer = runtime->executor->timers.back();
        assert(timer->delay == std::chrono::seconds(300) && timer->waits == 1);
        timer->fire(); // success is already queued, so cancel cannot change it
        server.reset();
        runtime->executor->drain();
        assert(timer->waits == 1 && timer->cancels == 1 && worker->tasks.empty());
    }
    {
        auto server = make_server(); server->start(); runtime->executor->drain();
        auto timer = runtime->executor->timers.back();
        timer->fire(); runtime->executor->drain();
        assert(timer->waits == 2 && worker->tasks.size() == 1);
        worker->drain();
        server.reset(); runtime->executor->drain();
        assert(timer->cancels == 1 && worker->tasks.empty());
    }
}
class GatedConnection final : public mail_system::MockConnection {
public:
    using MockConnection::MockConnection;
    void async_handshake(pr::net::HandshakeRole role, HandshakeHandler handler) override {
        assert(role == pr::net::HandshakeRole::server);
        completion = std::move(handler);
    }
    void async_write(pr::net::ConstBuffer buffer, WriteHandler handler) override {
        assert(!writing && "overlapping asynchronous writes on a POP3 connection");
        writing = true; ++writes;
        MockConnection::async_write(buffer, [this, handler = std::move(handler)](const std::error_code& error, size_t size) {
            writing = false; handler(error, size);
        });
    }
    bool writing = false;
    int writes = 0;
    HandshakeHandler completion;
};
void test_handshake_before_banner() {
    using namespace mail_system;
    for (bool succeeds : {true, false}) {
        auto executor = std::make_shared<ManualExecutor>();
        auto worker = std::make_shared<QueuedPool>();
        auto fsm = std::make_shared<TraditionalPop3Fsm<GatedConnection>>(worker, worker, make_router());
        auto connection = std::make_unique<GatedConnection>(executor);
        auto* wire = connection.get(); wire->set_deferred_read(true);
        auto session = std::make_shared<Pop3Session<GatedConnection>>(nullptr, std::move(connection), fsm);
        Pop3Session<GatedConnection>::start(session);
        executor->drain();
        assert(wire->completion && wire->written().empty() && !wire->has_pending_read());
        executor->post([&] {
            auto complete = std::move(wire->completion);
            complete(succeeds ? std::error_code{} : std::make_error_code(std::errc::protocol_error));
        });
        executor->drain();
        if (succeeds) assert(wire->written().find("+OK") == 0 && wire->has_pending_read());
        else assert(session->is_closed() && wire->written().empty());
        session->set_trace_clean_close(); session->close(); executor->drain();
        session.reset(); executor->drain();
    }
}
void test_multiline_response_is_one_write() {
    using namespace mail_system;
    auto executor = std::make_shared<ManualExecutor>();
    auto worker = std::make_shared<QueuedPool>();
    auto fsm = std::make_shared<TraditionalPop3Fsm<GatedConnection>>(worker, worker, make_router());
    auto connection = std::make_unique<GatedConnection>(executor);
    auto* wire = connection.get(); wire->set_deferred_read(true); wire->set_deferred_write(true);
    auto session = std::make_shared<Pop3Session<GatedConnection>>(nullptr, std::move(connection), fsm);
    session->set_current_state(static_cast<int>(Pop3State::TRANSACTION));
    auto* context = static_cast<Pop3Context*>(session->get_context());
    for (int i = 1; i <= 2; ++i) {
        Pop3Message message; message.mail_id = i; message.size = 100;
        context->messages.push_back(message);
    }
    session->do_async_read(); executor->drain();
    int commands = 0;
    for (const auto* command : {"CAPA\r\n", "LIST\r\n", "UIDL\r\n"}) {
        assert(wire->has_pending_read());
        wire->trigger_deferred_read(command); executor->drain();
        assert(wire->writing && wire->writes == ++commands && !wire->has_pending_read());
        const auto response = wire->written();
        assert(response.size() >= 3 && response.substr(response.size() - 3) == ".\r\n");
        wire->trigger_deferred_write(); executor->drain();
        assert(!wire->writing && wire->has_pending_read());
        wire->clear_written();
    }
    session->set_trace_clean_close(); session->close(); executor->drain();
    session.reset(); executor->drain();
}
void test_heartbeat_affinity() {
    using namespace mail_system;
    auto executor = std::make_shared<ManualExecutor>();
    auto worker = std::make_shared<QueuedPool>();
    auto database = std::make_shared<test::MockDbPool>();
    database->mock_conn()->set_deferred(false);
    auto router = make_router(database);
    using Fsm = TraditionalPop3Fsm<MockConnection>;
    auto fsm = std::make_shared<Fsm>(worker, worker, router);
    const auto interval = std::chrono::milliseconds(5);
    fsm->heartbeat_interval_ = interval;
    AuthCacheEntry auth;
    auth.password_hash = "test123"; auth.status = 1; auth.user_id = 42;
    fsm->m_authCache->inject("alice@test.local", auth);
    auto login = [&] {
        auto connection = std::make_unique<MockConnection>(executor);
        connection->set_deferred_read(true);
        auto session = std::make_shared<Pop3Session<MockConnection>>(nullptr, std::move(connection), fsm);
        static_cast<Pop3Context*>(session->get_context())->username = "alice@test.local";
        session->set_current_state(static_cast<int>(Pop3State::AUTHORIZATION));
        session->parse_pop3_command("PASS test123\r\n");
        database->mock_conn()->push_sync_result(std::make_shared<test::MockDbResult>(
            std::vector<std::map<std::string, std::string>>{{{"id", "10"}}}));
        database->mock_conn()->push_sync_result(std::make_shared<test::MockDbResult>(
            std::vector<std::map<std::string, std::string>>{{{"cnt", "1"}}}));
        database->mock_conn()->push_sync_result(std::make_shared<test::MockDbResult>());
        fsm->process_event(session, Pop3Event::PASS);
        worker->drain();
        assert(session->get_current_state() == static_cast<int>(Pop3State::TRANSACTION));
        return session;
    };
    auto session = login();
    auto* context = static_cast<Pop3Context*>(session->get_context());
    assert(executor->timers.empty());
    executor->drain();
    auto timer = executor->timers.front();
    assert(timer->waits == 1 && context->heartbeat_timer == timer);
    auto renew = [&] {
        database->mock_conn()->push_sync_result(std::make_shared<test::MockDbResult>(
            std::vector<std::map<std::string, std::string>>{{{"cnt", "1"}}}));
        timer->fire(); executor->drain();
        assert(worker->tasks.size() == 1);
        worker->drain(); // DB callback runs outside the connection executor.
    };
    renew();
    assert(timer->waits == 1); // Rearm must still be queued.
    executor->drain();
    assert(timer->waits == 2);
    renew();
    session->set_trace_clean_close();
    session->close(); // Close before the successful DB completion can rearm.
    assert(timer->cancels == 0);
    executor->drain();
    assert(timer->waits == 2 && timer->cancels == 1 && !context->heartbeat_timer);
    session.reset(); executor->drain();

    // An armed heartbeat must also cancel safely when the last owner drops it.
    auto abandoned = login();
    executor->drain();
    auto abandoned_timer = executor->timers.back();
    std::weak_ptr<Pop3Session<MockConnection>> weak = abandoned;
    abandoned->set_trace_clean_close();
    abandoned->get_connection().close(); // Release the mock's pending-read owner.
    abandoned.reset();
    assert(weak.expired() && abandoned_timer->cancels == 0);
    executor->drain();
    assert(abandoned_timer->cancels == 1);
}
}
int main() {
    pr::Logger::get_instance().set_handler({});
    test_sweeper_lifetime();
    test_handshake_before_banner();
    test_multiline_response_is_one_write();
    test_heartbeat_affinity();
}
