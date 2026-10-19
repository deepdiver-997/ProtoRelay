// 2026-10-18：after_persist 的 worker sleep 轮询会耗尽工作池，使持久化本身
// 无线程执行；见 docs/bugfixes/2026-10-18-persist-completion-callback.md。
#undef NDEBUG
#include <cassert>
#include "mail_system/back/mailServer/smtps_server.h"
#include "mail_system/back/mailServer/fsm/smtps/traditional_smtps_fsm.tpp"
#include "mail_system/back/mailServer/session/smtps_session.tpp"
#include "framework/session_base.tpp"
#include "framework/thread_pool/io_thread_pool.h"
#include "framework/thread_pool/worker_thread_pool.h"
#include "mail_system/back/router/static_shard_router.h"
#include "mail_system/back/storage/local_file_storage_provider.h"
#include "mock_connection.h"
#include <condition_variable>
#include <filesystem>
#include <future>
#include <iostream>
#include <unistd.h>
using namespace mail_system;
namespace pq = persist_storage;

// 延迟 dedicated queue thread 的首次投递，确定性复刻“等待先入池、持久化后入池”。
class GatePool : public pr::ThreadPoolBase {
public:
    GatePool() : controller_(std::this_thread::get_id()), actual_(1) {}
    void start() override { actual_.start(); }
    void stop(bool wait = true) override { actual_.stop(wait); }
    size_t thread_count() const override { return 1; }
    bool is_running() const override { return actual_.is_running(); }
    bool wait_held() {
        std::unique_lock<std::mutex> lock(mutex_);
        return ready_.wait_for(lock, std::chrono::seconds(2), [&] { return bool(held_); });
    }
    void release() {
        std::function<void()> job;
        { std::lock_guard<std::mutex> lock(mutex_); job = std::move(held_); }
        if (job) actual_.post(std::move(job));
    }
protected:
    void post_impl(std::function<void()> job) override {
        { std::lock_guard<std::mutex> lock(mutex_);
          if (armed_ && std::this_thread::get_id() != controller_) {
              armed_ = false; held_ = std::move(job); ready_.notify_all(); return;
          }
        }
        actual_.post(std::move(job));
    }
private:
    std::thread::id controller_;
    pr::WorkerThreadPool actual_;
    std::mutex mutex_;
    std::condition_variable ready_;
    bool armed_ = true;
    std::function<void()> held_;
};
struct TestServer : SmtpsServer {
    using SmtpsServer::SmtpsServer;
    void start() override {}
};
enum class Scenario { Success, Closed, Timeout };
bool run(Scenario scenario) {
    const auto directory = std::filesystem::temp_directory_path() / ("smtp-persist-ack-" + std::to_string(getpid()));
    auto io = std::make_shared<pr::IOThreadPool>(1); io->start();
    auto workers = std::make_shared<GatePool>(); workers->start();
    auto storage = std::make_shared<storage::LocalFileStorageProvider>((directory / "mail").string(), (directory / "att").string());
    auto router = std::make_shared<router::StaticShardRouter>(std::vector<std::pair<std::string,int>>{}, 0,
        std::vector<std::shared_ptr<DBPool>>{}, std::vector<std::shared_ptr<storage::IStorageProvider>>{storage});
    auto queue = std::make_shared<pq::PersistentQueue>(router, workers);
    queue->set_pressure_config({100, 0, 0});
    ServerConfig cfg; cfg.perf_mode = true; cfg.apply_perf_mode(); cfg.use_database = false;
    cfg.inbound_ack_mode = InboundAckMode::AFTER_PERSIST; cfg.inbound_persist_wait_timeout_ms = scenario == Scenario::Timeout ? 30 : 1000;
    cfg.log_to_file = false; cfg.log_to_console = false;
    auto server = std::make_shared<TestServer>(cfg, io, workers, router); server->m_persistentQueue = queue;
    auto fsm = std::make_shared<TraditionalSmtpsFsm<MockConnection>>(io, workers, queue, router);
    auto connection = std::make_unique<MockConnection>(); auto* wire = connection.get();
    wire->set_deferred_read(true); wire->start_executor();
    auto session = std::make_shared<SmtpsSession<MockConnection>>(server.get(), std::move(connection), fsm);
    auto* context = static_cast<SmtpsContext*>(session->get_context());
    context->sender_address = "sender@test.local"; context->recipient_addresses = {"rcpt@test.local"};
    session->set_current_state(static_cast<int>(SmtpsState::IN_MESSAGE));
    session->create_mail_on_data_command(); session->parse_smtp_command("Subject: test\r\n\r\nhello\r\n.\r\n");
    const auto body = session->get_mail()->body_path;
    fsm->process_event(session, SmtpsEvent::DATA_END);
    assert(workers->wait_held());
    assert(wire->written().find("250 OK") == std::string::npos);
    std::promise<void> probe; auto available = probe.get_future();
    workers->post([&] { probe.set_value(); });
    const bool worker_free = available.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready;
    if (scenario == Scenario::Closed) {
        session->close(); wire->pump_executor(); assert(session->is_closed());
    }
    if (scenario == Scenario::Timeout) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < until && wire->written().empty())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        assert(wire->written().find("451 Requested action aborted: local processing timeout") != std::string::npos);
    }
    workers->release();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline && queue->inflight_count())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(queue->inflight_count() == 0);
    // Queue completion precedes response delivery. With a serialized connection
    // executor, poll() on a second thread cannot drain a strand already running
    // on its owner. Wait for the consumer-visible response and accepted metric.
    if (scenario == Scenario::Success) {
        const auto response_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < response_deadline &&
               (wire->written().empty() || server->mails_accepted_total_.load() != 1))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    wire->pump_executor();
    const auto response = wire->written();
    const bool kept_body = std::filesystem::exists(body);
    std::ifstream input(body, std::ios::binary);
    const std::string stored((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const auto accepted = server->mails_accepted_total_.load();
    finish_session(session);
    queue->shutdown(); workers->stop(); io->stop();
    fsm.reset(); server.reset(); queue.reset();
    std::filesystem::remove_all(directory);
    if (!worker_free) { std::cerr << "FAIL: after_persist occupied the only worker while persistence was waiting\n"; return false; }
    if (scenario == Scenario::Success) assert(response == "250 OK\r\n" && kept_body && stored.find("hello") != std::string::npos && accepted == 1);
    if (scenario == Scenario::Closed) assert(response.empty() && kept_body && stored.find("hello") != std::string::npos && accepted == 0);
    if (scenario == Scenario::Timeout) assert(response.find("451 ") == 0 && !kept_body && accepted == 0);
    return true;
}
int main() {
    if (!run(Scenario::Success)) return 1;
    assert(run(Scenario::Closed));
    assert(run(Scenario::Timeout));
    std::cout << "PASS: after_persist frees worker, waits for completion, handles closed sessions and timeout\n";
}
