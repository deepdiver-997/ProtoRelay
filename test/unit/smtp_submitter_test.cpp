// webmail submission：真实 loopback TCP 对话；脚本 SMTP 对端验证 AUTH/envelope/
// DATA 消费结果，覆盖拒绝/超时/结果不确定。无需 DB，不向外网发邮件。
#undef NDEBUG
#include <cassert>
#include "mail_system/back/webmail/smtp_submitter.h"
#include "mail_system/back/common/mail_crypto.h"
#include "mail_system/back/common/mime_composer.h"
#include "mail_system/back/common/smtp_wire.h"

#include <boost/asio.hpp>
#include <fstream>
#include <iostream>
#include <thread>

using namespace mail_system;
using namespace mail_system::webmail;
using boost::asio::ip::tcp;

namespace {

struct Step {
    std::string expected;
    std::string reply;
    bool data = false;
    bool disconnect = false;
};

// 每段都通过 async_read_until 读取，DATA 帧与命令分别验证；最后一段空 reply
// 留给客户端 deadline 触发，disconnect 模拟接受 DATA 后失去最终响应。
class ScriptServer : public std::enable_shared_from_this<ScriptServer> {
public:
    ScriptServer(boost::asio::io_context& io, std::vector<Step> script, std::string greeting)
        : acceptor_(io, tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0)),
          socket_(io), script_(std::move(script)), greeting_(std::move(greeting)) {}
    uint16_t port() const { return acceptor_.local_endpoint().port(); }
    size_t completed_steps = 0;
    void start() {
        auto self = shared_from_this();
        acceptor_.async_accept(socket_, [self](auto ec) {
            assert(!ec);
            if (self->greeting_.empty()) return;
            self->write(self->greeting_);
        });
    }
private:
    void write(std::string reply) {
        auto self = shared_from_this();
        writing_ = std::move(reply);
        boost::asio::async_write(socket_, boost::asio::buffer(writing_), [self](auto ec, size_t) {
            assert(!ec);
            self->read();
        });
    }
    void read() {
        if (completed_steps == script_.size()) return;
        const auto& step = script_[completed_steps];
        auto self = shared_from_this();
        boost::asio::async_read_until(socket_, input_, step.data ? "\r\n.\r\n" : "\r\n",
            [self](auto ec, size_t size) {
                assert(!ec);
                std::string actual(size, '\0');
                std::istream input(&self->input_);
                input.read(actual.data(), static_cast<std::streamsize>(size));
                const auto& step = self->script_[self->completed_steps++];
                assert(actual == step.expected);
                if (step.disconnect) { self->socket_.close(); return; }
                if (!step.reply.empty()) self->write(step.reply);
            });
    }
    tcp::acceptor acceptor_;
    tcp::socket socket_;
    boost::asio::streambuf input_;
    std::vector<Step> script_;
    std::string greeting_, writing_;
};

std::string auth_token() {
    const std::string plain = std::string(1, '\0') + "sender@test.local" + '\0' + "secret";
    return outbound::base64_encode(reinterpret_cast<const unsigned char*>(plain.data()), plain.size()) + "\r\n";
}

std::vector<Step> success_script() {
    return {
        {"EHLO webmail.local\r\n", "250-test.local\r\n250-AUTH LOGIN PLAIN\r\n250 SIZE 10240000\r\n"},
        {"AUTH PLAIN\r\n", "334 \r\n"},
        {auth_token(), "235 Authentication successful\r\n"},
        {"MAIL FROM:<sender@test.local>\r\n", "250 Sender OK\r\n"},
        {"RCPT TO:<a@test.local>\r\n", "250 Recipient OK\r\n"},
        {"RCPT TO:<hidden@test.local>\r\n", "251 Forwarded\r\n"},
        {"DATA\r\n", "354 Start mail\r\n"},
        {"..first\r\n..\r\nlast\r\n.\r\n", "250 Persisted\r\n", true},
        {"QUIT\r\n", "221 Bye\r\n"},
    };
}

SubmitResult run(std::vector<Step> steps, std::string greeting = "220 test.local\r\n",
                  std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    boost::asio::io_context io;
    auto server = std::make_shared<ScriptServer>(io, std::move(steps), std::move(greeting));
    server->start();
    LoopbackSmtpSubmitter submitter(io.get_executor(), server->port(), timeout);
    SubmitRequest request{"sender@test.local", std::make_shared<SessionPassword>("secret"),
                           {"a@test.local", "hidden@test.local"}, ".first\r\n.\r\nlast\r\n"};
    int calls = 0;
    SubmitResult result;
    submitter.submit_async(std::move(request), [&](SubmitResult out) { ++calls; result = std::move(out); });
    // 两条线程共享 executor；客户端 strand 保证 timer/socket 单对象串行。
    std::thread other([&] { io.run(); });
    io.run();
    other.join();
    assert(calls == 1);
    return result;
}

void test_executor_provider() {
    boost::asio::io_context peers_io, first_io, second_io;
    auto first_guard = boost::asio::make_work_guard(first_io);
    auto second_guard = boost::asio::make_work_guard(second_io);
    std::atomic<int> selections{0}, calls{0}, thread_mask{0};
    std::vector<std::shared_ptr<ScriptServer>> peers;
    for (int i = 0; i < 4; ++i) {
        peers.push_back(std::make_shared<ScriptServer>(peers_io, success_script(), "220 test.local\r\n"));
        peers.back()->start();
        auto submitter = std::make_shared<LoopbackSmtpSubmitter>([&]() -> boost::asio::any_io_executor {
            return selections.fetch_add(1) % 2 ? second_io.get_executor() : first_io.get_executor();
        }, peers.back()->port());
        submitter->submit_async({"sender@test.local", std::make_shared<SessionPassword>("secret"),
                                {"a@test.local", "hidden@test.local"}, ".first\r\n.\r\nlast\r\n"},
            [&](SubmitResult result) {
                assert(result.status == SubmitStatus::Accepted);
                const int bit = first_io.get_executor().running_in_this_thread() ? 1 :
                                second_io.get_executor().running_in_this_thread() ? 2 : 0;
                thread_mask.fetch_or(bit);
                if (calls.fetch_add(1) == 3) { first_guard.reset(); second_guard.reset(); }
            });
    }
    std::thread first([&] { first_io.run(); });
    std::thread second([&] { second_io.run(); });
    peers_io.run(); first.join(); second.join();
    assert(selections == 4 && calls == 4 && thread_mask == 3);
    for (const auto& peer : peers) assert(peer->completed_steps == success_script().size());
    std::cout << "  [PASS] executor provider: concurrent submissions distributed across two I/O contexts\n";
}

void test_protocol() {
    auto result = run(success_script());
    assert(result.status == SubmitStatus::Accepted);
    auto steps = success_script();
    steps[2].reply = "535 Authentication failed\r\n";
    steps.resize(3);
    result = run(steps);
    assert(result.status == SubmitStatus::AuthFailed && result.smtp_code == 535);
    steps = success_script();
    steps[3].reply = "550 Daily quota exceeded\r\n";
    steps.resize(4);
    result = run(steps);
    assert(result.status == SubmitStatus::Rejected && result.stage == "mail");
    steps = success_script();
    steps[5].reply = "550 No such recipient\r\n";
    steps.resize(6);
    result = run(steps);
    assert(result.status == SubmitStatus::Rejected && result.stage == "recipient");
    // 部分 RCPT 接受也不发送 DATA；不会偷偷提交只给部分收件人的邮件。
    steps = success_script();
    steps[4].reply = "450 Try again\r\n";
    steps.resize(5);
    result = run(steps);
    assert(result.status == SubmitStatus::Unavailable && result.smtp_code == 450);
    steps = success_script();
    steps[7].reply = "451 Storage unavailable\r\n";
    steps.resize(8);
    result = run(steps);
    assert(result.status == SubmitStatus::Unavailable && result.smtp_code == 451);
    steps[7].reply = "552 Too large\r\n";
    result = run(steps);
    assert(result.status == SubmitStatus::Rejected && result.smtp_code == 552);

    steps = success_script();
    steps[7].disconnect = true;
    steps.resize(8);
    result = run(steps);
    assert(result.status == SubmitStatus::OutcomeUnknown && result.stage == "body");
    steps[7].disconnect = false;
    steps[7].reply.clear();
    result = run(steps, "220 test.local\r\n", std::chrono::milliseconds(100));
    assert(result.status == SubmitStatus::OutcomeUnknown);
    result = run({}, "", std::chrono::milliseconds(100));
    assert(result.status == SubmitStatus::Unavailable && result.stage == "connect");

    steps = success_script();
    steps[0].reply = "250 test.local\r\n"; steps.resize(1);
    result = run(steps);
    assert(result.status == SubmitStatus::ProtocolError); // 不能拿无 AUTH 端口绕过配额
    steps[0].reply = "250-test.local\r\n251 inconsistent\r\n";
    result = run(steps);
    assert(result.status == SubmitStatus::ProtocolError);
    steps[0].reply = "garbage\r\n";
    result = run(steps);
    assert(result.status == SubmitStatus::ProtocolError);
    std::cout << "  [PASS] loopback SMTP: AUTH, envelope, DATA, policy, timeout, unknown outcome\n";
}

mime::Message sample() {
    mime::Message m;
    m.from = "sender@test.local";
    m.to = {"a@test.local"};
    m.cc = {"cc@test.local"};
    m.to.push_back("A@TEST.LOCAL");
    for (int i = 0; i < 20; ++i) m.subject += "你好，Webmail！";
    m.text = "你好\n.\nlast";
    m.html = "<p>你好 &amp; hello</p>";
    return m;
}

void test_composer() {
    auto m = sample();
    const auto composed = mime::compose(m);
    assert(!composed.error_status && !composed.raw.empty());
    assert(composed.recipients == std::vector<std::string>({"a@test.local", "cc@test.local"}));
    assert(composed.raw.find("hidden@test.local") == std::string::npos);
    assert(composed.message_id != mime::compose(m).message_id);
    for (const auto& address : {"a@test.local", "first.last+tag@test.local", "a@localhost"}) assert(mime::valid_address(address));
    for (const auto& address : {"", "a@", "@test.local", "a..b@test.local", "a@.local", "a@-bad.local",
                               "a@bad-.local", "a@@test.local", "A <a@test.local>", "a@test.local\r\nDATA"})
        assert(!mime::valid_address(address));
    m.from = "a@test.local\r\nBcc: bad@test.local";
    assert(mime::compose(m).error_status == 400);
    m = sample(); m.to.assign(101, "a@test.local");
    assert(mime::compose(m).error_status == 400);
    m = sample(); m.subject = "hello\r\nBcc: bad@test.local";
    assert(mime::compose(m).error_status == 400);
    m = sample(); m.text = std::string(mime::kMaxBodyBytes + 1, 'x');
    assert(mime::compose(m).error_status == 413);
    assert(smtp::data_frame(".\r\n..\r\n") == "..\r\n...\r\n.\r\n");
    assert(smtp::data_frame("x") == "x\r\n.\r\n");
    std::cout << "  [PASS] MIME compose validation and DATA transparency\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--emit-mime") {
        const auto composed = mime::compose(sample());
        assert(!composed.error_status);
        std::ofstream file(argv[2], std::ios::binary);
        file << composed.raw;
        return file ? 0 : 1;
    }
    test_composer();
    test_protocol();
    test_executor_provider();
    std::cout << "All SMTP submission tests passed!\n";
}
