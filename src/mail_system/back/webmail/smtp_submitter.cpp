#include "mail_system/back/webmail/smtp_submitter.h"
#include "mail_system/back/common/logger.h"
#include "mail_system/back/common/mail_crypto.h"
#include "mail_system/back/common/mime_composer.h"
#include "mail_system/back/common/smtp_wire.h"

#include <boost/asio.hpp>
#include <cctype>
#include <istream>
#include <memory>
#include <openssl/crypto.h>
#include <sstream>

namespace mail_system::webmail {
namespace {

using boost::asio::ip::tcp;

class Submission : public std::enable_shared_from_this<Submission> {
public:
    Submission(boost::asio::any_io_executor executor, uint16_t port,
               std::chrono::milliseconds timeout, SubmitRequest request, ISmtpSubmitter::Complete cb)
        : socket_(executor), timer_(executor), port_(port), timeout_(timeout),
          request_(std::move(request)), complete_(std::move(cb)), replies_(64 * 1024) {}

    ~Submission() {
        if (stage_ == Stage::Credentials && !write_.empty()) OPENSSL_cleanse(write_.data(), write_.size());
    }

    void start() {
        // 防御 DI 直接调用：即使绕过 API/composer 也不允许命令注入。
        if (!mime::valid_address(request_.sender) || !request_.password ||
            request_.password->view().empty() || request_.password->view().size() > 4096 ||
            request_.password->view().find('\0') != std::string_view::npos ||
            request_.recipients.empty() || request_.recipients.size() > mime::kMaxRecipients ||
            request_.raw.empty()) {
            finish(SubmitStatus::ProtocolError); return;
        }
        for (const auto& rcpt : request_.recipients)
            if (!mime::valid_address(rcpt)) { finish(SubmitStatus::ProtocolError); return; }
        auto self = shared_from_this();
        timer_.expires_after(timeout_);
        timer_.async_wait([self](const boost::system::error_code& ec) {
            if (!ec) self->io_failed("deadline exceeded");
        });
        socket_.async_connect(tcp::endpoint(boost::asio::ip::address_v4::loopback(), port_),
            [self](const boost::system::error_code& ec) {
                if (self->done_) return;
                if (ec) { self->io_failed(ec.message()); return; }
                self->read_reply();
            });
    }

private:
    enum class Stage { Greeting, Ehlo, Auth, Credentials, Mail, Recipient, Data, Body, Quit };

    const char* stage_name() const {
        switch (stage_) {
            case Stage::Greeting: return "connect";
            case Stage::Ehlo: return "ehlo";
            case Stage::Auth: case Stage::Credentials: return "auth";
            case Stage::Mail: return "mail";
            case Stage::Recipient: return "recipient";
            case Stage::Data: return "data";
            case Stage::Body: return "body";
            case Stage::Quit: return "quit";
        }
        return "unknown";
    }

    void finish(SubmitStatus status, int code = 0) {
        if (done_) return;
        done_ = true;
        timer_.cancel();
        boost::system::error_code ignored;
        socket_.close(ignored);
        request_.password.reset();
        // write_ 可能仍被异步写引用；只在写回调/析构中擦除，不能在超时路径改缓冲。
        if (status != SubmitStatus::Accepted)
            LOG_AT("WEBAPI", warn, "submission failed: stage={} smtp_code={} status={}",
                   stage_name(), code, static_cast<int>(status));
        auto cb = std::move(complete_);
        cb({status, code, stage_name()});
    }

    void io_failed(const std::string& reason) {
        if (done_) return;
        LOG_AT("WEBAPI", warn, "submission I/O: stage={} reason={}", stage_name(), reason);
        finish(accepted_ ? SubmitStatus::Accepted :
               data_started_ ? SubmitStatus::OutcomeUnknown : SubmitStatus::Unavailable);
    }

    void send(Stage stage, std::string wire) {
        stage_ = stage;
        write_ = std::move(wire);
        auto self = shared_from_this();
        boost::asio::async_write(socket_, boost::asio::buffer(write_),
            [self](const boost::system::error_code& ec, size_t) {
                if (self->stage_ == Stage::Credentials && !self->write_.empty())
                    OPENSSL_cleanse(self->write_.data(), self->write_.size());
                self->write_.clear();
                if (self->done_) return;
                if (ec) { self->io_failed(ec.message()); return; }
                if (self->stage_ == Stage::Quit) {
                    self->finish(SubmitStatus::Accepted, 250); return;
                }
                self->reply_code_ = 0;
                self->reply_bytes_ = 0;
                self->read_reply();
            });
    }

    void read_reply() {
        auto self = shared_from_this();
        boost::asio::async_read_until(socket_, replies_, "\r\n",
            [self](const boost::system::error_code& ec, size_t count) {
                if (self->done_) return;
                if (ec) { self->io_failed(ec.message()); return; }
                std::string line(count, '\0');
                std::istream input(&self->replies_);
                input.read(line.data(), static_cast<std::streamsize>(count));
                self->reply_bytes_ += count;
                if (count < 6 || self->reply_bytes_ > 64 * 1024 ||
                    line[0] < '2' || line[0] > '5' || line[1] < '0' || line[1] > '9' ||
                    line[2] < '0' || line[2] > '9' || (line[3] != ' ' && line[3] != '-')) {
                    self->protocol_failed(); return;
                }
                const int code = (line[0] - '0') * 100 + (line[1] - '0') * 10 + line[2] - '0';
                if (self->reply_code_ && self->reply_code_ != code) {
                    self->protocol_failed(); return;
                }
                self->reply_code_ = code;
                if (self->stage_ == Stage::Ehlo) {
                    // 只选服务器明确通告的 AUTH PLAIN，不能把无认证的 25 当提交口。
                    auto capability = line.substr(4, line.size() - 6);
                    for (auto& c : capability) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    if (capability.rfind("AUTH ", 0) == 0 || capability.rfind("AUTH=", 0) == 0) {
                        std::istringstream mechanisms(capability.substr(5));
                        std::string mechanism;
                        while (mechanisms >> mechanism) if (mechanism == "PLAIN") self->auth_plain_ = true;
                    }
                }
                if (line[3] == '-') { self->read_reply(); return; }
                self->on_reply(code);
            });
    }

    void on_reply(int code) {
        if (code >= 400) {
            if (code == 535 && (stage_ == Stage::Auth || stage_ == Stage::Credentials))
                finish(SubmitStatus::AuthFailed, code);
            else finish(code < 500 ? SubmitStatus::Unavailable : SubmitStatus::Rejected, code);
            return;
        }
        const int expected = stage_ == Stage::Greeting ? 220 :
            stage_ == Stage::Auth ? 334 : stage_ == Stage::Credentials ? 235 :
            stage_ == Stage::Data ? 354 : 250;
        if (code != expected && !(stage_ == Stage::Recipient && code == 251)) {
            protocol_failed(code); return;
        }
        switch (stage_) {
            case Stage::Greeting: send(Stage::Ehlo, "EHLO webmail.local\r\n"); break;
            case Stage::Ehlo:
                if (!auth_plain_) { finish(SubmitStatus::ProtocolError, code); return; }
                send(Stage::Auth, "AUTH PLAIN\r\n"); break;
            case Stage::Auth: {
                std::string plain(1, '\0');
                plain += request_.sender;
                plain += '\0';
                plain += request_.password->view();
                auto encoded = outbound::base64_encode(
                    reinterpret_cast<const unsigned char*>(plain.data()), plain.size());
                OPENSSL_cleanse(plain.data(), plain.size());
                request_.password.reset();
                send(Stage::Credentials, std::move(encoded) + "\r\n");
                break;
            }
            case Stage::Credentials: send(Stage::Mail, "MAIL FROM:<" + request_.sender + ">\r\n"); break;
            case Stage::Mail: send_recipient(); break;
            case Stage::Recipient:
                if (++recipient_ < request_.recipients.size()) send_recipient();
                else send(Stage::Data, "DATA\r\n");
                break;
            case Stage::Data:
                // 从此刻起任何 I/O 故障都可能已投递，不自动重试。
                data_started_ = true;
                send(Stage::Body, smtp::data_frame(request_.raw));
                break;
            case Stage::Body:
                accepted_ = true;
                send(Stage::Quit, "QUIT\r\n");
                break;
            case Stage::Quit: break;
        }
    }

    void send_recipient() {
        send(Stage::Recipient, "RCPT TO:<" + request_.recipients[recipient_] + ">\r\n");
    }

    void protocol_failed(int code = 0) {
        finish(data_started_ ? SubmitStatus::OutcomeUnknown : SubmitStatus::ProtocolError, code);
    }

    tcp::socket socket_;
    boost::asio::steady_timer timer_;
    uint16_t port_;
    std::chrono::milliseconds timeout_;
    SubmitRequest request_;
    ISmtpSubmitter::Complete complete_;
    boost::asio::streambuf replies_;
    std::string write_;
    Stage stage_ = Stage::Greeting;
    size_t recipient_ = 0, reply_bytes_ = 0;
    int reply_code_ = 0;
    bool auth_plain_ = false, data_started_ = false, accepted_ = false, done_ = false;
};

} // namespace

LoopbackSmtpSubmitter::LoopbackSmtpSubmitter(boost::asio::any_io_executor executor,
                                           uint16_t port, std::chrono::milliseconds timeout)
    : executor_(std::move(executor)), port_(port), timeout_(timeout) {}

void LoopbackSmtpSubmitter::submit_async(SubmitRequest request, Complete complete) {
    auto strand = boost::asio::make_strand(executor_);
    auto submission = std::make_shared<Submission>(strand, port_, timeout_, std::move(request), std::move(complete));
    boost::asio::post(strand, [submission] { submission->start(); });
}

} // namespace mail_system::webmail
