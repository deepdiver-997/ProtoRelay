// 2026-10-18：旧 outbound 在 MAIL/RCPT/DATA 的拒绝和连接重试耗尽时不完成任务，
// 只靠租约超时恢复；此次迁移必须逐任务回调。回归与负验证见
// docs/bugfixes/2026-10-18-outbound-result-completion.md。
#undef NDEBUG
#include <cassert>
#include <iostream>
#include "smtp_test_peer.h"
#include "mail_system/back/mailServer/session/outbound_smtp_session.h"
#include <boost/asio/thread_pool.hpp>
using namespace mail_system;
using namespace mail_system::outbound;
using namespace smtp_test;

std::vector<Step> tx(std::string result = "250 Stored\r\n") {
    return {{"MAIL FROM:<sender@test.local>\r\n", "250 Sender\r\n"},
            {"RCPT TO:<a@test.local>\r\n", "250 Recipient\r\n"},
            {"DATA\r\n", "354 Send\r\n"},
            {"Subject: outbound\r\n\r\n..payload\r\n.\r\n", std::move(result), true}};
}
std::unique_ptr<MailDeliveryTask> task(uint64_t id) {
    auto task = std::make_unique<MailDeliveryTask>();
    task->mail_id = id; task->record_id = id;
    task->sender = "sender@test.local"; task->recipient = "a@test.local";
    task->mail_ptr = std::make_shared<::mail>();
    task->mail_ptr->body = "Subject: outbound\r\n\r\n.payload\r\n";
    return task;
}
std::shared_ptr<OutboundSmtpSession> session(asio::io_context& io, asio::thread_pool& worker, uint16_t port,
                                              asio::io_context* connection_io = nullptr) {
    auto selections = std::make_shared<std::atomic<int>>(0);
    return std::make_shared<OutboundSmtpSession>(
        [&io, connection_io, selections]() -> asio::any_io_executor {
            return connection_io && selections->fetch_add(1) > 0 ? connection_io->get_executor() : io.get_executor();
        },
        [&worker](std::function<void()> job) { asio::post(worker, std::move(job)); },
        "127.0.0.1", port, "outbound.test");
}
void reuse() {
    asio::io_context io, connection_io; asio::thread_pool worker(2);
    auto connection_guard = asio::make_work_guard(connection_io);
    std::thread connection_thread([&] { connection_io.run(); });
    auto guard = asio::make_work_guard(io);
    std::vector<Step> steps{{"EHLO outbound.test\r\n", "250-test\r\n250 SIZE 10000\r\n"}};
    for (int i = 0; i < 2; ++i) { auto one = tx(); steps.insert(steps.end(), one.begin(), one.end()); }
    auto peer = std::make_shared<Peer>(io, steps); peer->start();
    auto client = session(io, worker, peer->port(), &connection_io);
    client->set_mx_pool({"::1", "127.0.0.1"}); // 首候选拒连，第二候选成功
    int calls = 0;
    client->set_completion_cb([&](uint64_t id, smtp::Result result) {
        assert(id == static_cast<uint64_t>(++calls));
        assert(result.outcome == smtp::Outcome::Accepted && result.code == 250);
        if (calls == 2) { client->close(); peer->stop(); guard.reset(); }
    });
    client->submit(task(1)); client->submit(task(2));
    io.run(); connection_guard.reset(); connection_thread.join(); worker.join();
    assert(calls == 2 && peer->completed == steps.size());
}
void failure(std::vector<Step> commands, smtp::Outcome outcome, int code, std::string stage) {
    asio::io_context io; asio::thread_pool worker(1); auto guard = asio::make_work_guard(io);
    commands.insert(commands.begin(), {"EHLO outbound.test\r\n", "250 Ready\r\n"});
    auto peer = std::make_shared<Peer>(io, commands); peer->start();
    auto client = session(io, worker, peer->port());
    int calls = 0;
    asio::steady_timer watchdog(io, std::chrono::seconds(3));
    watchdog.async_wait([&](auto ec) { if (!ec) { std::cerr << "outbound completion lost\n"; std::abort(); } });
    client->set_completion_cb([&](uint64_t id, smtp::Result r) {
        assert(id == 17); ++calls;
        assert(r.outcome == outcome && r.code == code && r.stage == stage);
        watchdog.cancel(); client->close(); peer->stop(); guard.reset();
    });
    client->submit(task(17)); io.run(); worker.join();
    assert(calls == 1 && peer->completed == commands.size());
}
void connect_exhausted() {
    asio::io_context io; asio::thread_pool worker(1); auto guard = asio::make_work_guard(io);
    // 释放一个本机端口，使三轮 connect 都得到 ECONNREFUSED。
    tcp::acceptor reserved(io); reserved.open(tcp::v4()); reserved.bind({asio::ip::address_v4::loopback(), 0});
    const auto port = reserved.local_endpoint().port(); reserved.close();
    auto client = session(io, worker, port);
    int calls = 0;
    asio::steady_timer watchdog(io, std::chrono::seconds(5));
    watchdog.async_wait([&](auto ec) { if (!ec) { std::cerr << "queued submit cancelled connection retry\n"; std::abort(); } });
    client->set_completion_cb([&](uint64_t id, smtp::Result r) {
        assert(id == static_cast<uint64_t>(++calls) && r.outcome == smtp::Outcome::TemporaryFailure && r.stage == "connect");
        if (calls == 2) { watchdog.cancel(); client->close(); guard.reset(); }
    });
    client->submit(task(1));
    // 重试退避中到达新任务，不能取消当前任务的 retry timer。
    asio::steady_timer enqueue(io, std::chrono::milliseconds(100));
    enqueue.async_wait([&](auto ec) { assert(!ec); client->submit(task(2)); });
    io.run(); worker.join(); assert(calls == 2);
}
int main() {
    reuse();
    auto commands = tx(); commands[1].reply = "550 Missing recipient\r\n"; commands.resize(2);
    failure(commands, smtp::Outcome::Rejected, 550, "recipient");
    commands[1].reply = "450 Later\r\n";
    failure(commands, smtp::Outcome::TemporaryFailure, 450, "recipient");
    commands = tx(); commands[0].reply = "550 Sender denied\r\n"; commands.resize(1);
    failure(commands, smtp::Outcome::Rejected, 550, "mail");
    commands = tx(); commands[2].reply = "451 Queue unavailable\r\n"; commands.resize(3);
    failure(commands, smtp::Outcome::TemporaryFailure, 451, "data");
    commands = tx("552 Too big\r\n");
    failure(commands, smtp::Outcome::Rejected, 552, "body");
    commands.back().disconnect = true;
    failure(commands, smtp::Outcome::OutcomeUnknown, 0, "body");
    connect_exhausted();
    std::cout << "Outbound: MX failover, reuse, envelope/DATA failure completion, unknown outcome, retries passed\n";
}
