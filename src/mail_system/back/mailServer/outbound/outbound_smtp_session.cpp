#include "mail_system/back/mailServer/session/outbound_smtp_session.h"
#include "mail_system/back/mailServer/outbound/outbound_utils.h"
#include "mail_system/back/mailServer/outbound/dns_resolver.h"
#include "mail_system/back/common/logger.h"
#include "framework/metrics_server.h"
#include <boost/asio.hpp>
#include <fstream>
#include <sstream>

namespace mail_system::outbound {
namespace asio = boost::asio;
OutboundSmtpSession::OutboundSmtpSession(smtp::ExecutorProvider executors, WorkerPost worker,
                                         std::string host, int port, std::string helo, std::weak_ptr<pr::MetricsServer> metrics)
    : executors_(std::move(executors)), worker_post_(std::move(worker)),
      executor_(asio::make_strand(executors_())), timer_(executor_),
      host_(std::move(host)), helo_(std::move(helo)), port_(port), metrics_(std::move(metrics)) {}
void OutboundSmtpSession::set_outbound_config(OutboundConfig config) {
    asio::post(executor_, [self = shared_from_this(), config = std::move(config)]() mutable {
        self->config_ = std::move(config);
    });
}
void OutboundSmtpSession::set_mx_pool(std::vector<std::string> pool) {
    asio::post(executor_, [self = shared_from_this(), pool = std::move(pool)]() mutable {
        self->mx_pool_ = std::move(pool);
    });
}
void OutboundSmtpSession::set_completion_cb(CompletionCb cb) {
    asio::post(executor_, [self = shared_from_this(), cb = std::move(cb)]() mutable { self->complete_ = std::move(cb); });
}
void OutboundSmtpSession::submit(std::unique_ptr<MailDeliveryTask> task) {
    pending_.fetch_add(1);
    asio::post(executor_, [self = shared_from_this(), task = std::move(task)]() mutable {
        if (self->stopped_) { self->pending_.fetch_sub(1); return; }
        if (!self->task_) self->timer_.cancel();
        self->queue_.push(std::move(task));
        self->pump();
    });
}
void OutboundSmtpSession::pump() {
    if (stopped_ || task_ || state_ == SessionState::CLOSING) return;
    if (queue_.empty()) { idle(); return; }
    task_ = std::move(queue_.front()); queue_.pop();
    active_ = true;
    state_ = SessionState::DELIVERING;
    retries_ = 0; mx_index_ = 0;
    if (task_->expired()) {
        complete({smtp::Outcome::TemporaryFailure, 0, "queue", "task TTL expired"}); return;
    }
    prepare();
}
void OutboundSmtpSession::prepare() {
    auto self = shared_from_this();
    auto mail = task_->mail_ptr;
    auto config = config_;
    const auto mail_id = task_->mail_id;
    worker_post_([self, mail, config, mail_id] {
        std::string raw, error;
        if (!mail) error = "mail payload missing";
        else if (!mail->body.empty()) raw = mail->body;
        else {
            std::ifstream file(mail->body_path, std::ios::binary);
            if (!file) error = "cannot open mail body";
            else { std::ostringstream stream; stream << file.rdbuf(); raw = stream.str(); }
        }
        if (raw.empty() && error.empty()) error = "empty mail body";
        if (error.empty()) {
            std::string signing_error;
            raw = sign_payload_if_dkim(raw, config, &signing_error);
            if (!signing_error.empty())
                LOG_SMTP_WARN("Outbound: DKIM sign skipped mail_id={} reason={}", mail_id, signing_error);
        }
        asio::post(self->executor_, [self, raw = std::move(raw), error = std::move(error)]() mutable {
            if (self->stopped_) return;
            if (!error.empty()) { self->complete({smtp::Outcome::TemporaryFailure, 0, "payload", std::move(error)}); return; }
            self->payload_ = std::make_shared<const std::string>(std::move(raw));
            if (self->client_ && self->client_->is_open()) self->send();
            else self->connect();
        });
    });
}
void OutboundSmtpSession::connect() {
    if (stopped_) return;
    state_ = SessionState::CONNECTING;
    // 每个新连接从 provider 选一次 executor，后续 I/O 不跨池迁移。
    client_ = std::make_shared<smtp::ClientSession>(executors_());
    smtp::ConnectOptions options;
    options.host = local_shortcut_if_enabled(mx_pool_.empty() ? host_ : mx_pool_[mx_index_ % mx_pool_.size()]);
    options.port = static_cast<uint16_t>(port_);
    options.helo = helo_;
    options.timeout = std::chrono::seconds(15);
    auto self = shared_from_this();
    const auto started = std::chrono::steady_clock::now();
    client_->async_connect(std::move(options), [self, started](smtp::Result result) {
        asio::post(self->executor_, [self, started, result = std::move(result)]() mutable {
            if (self->stopped_) return;
            if (result.outcome == smtp::Outcome::Ready) {
                self->sent_ = 0;
                if (auto metrics = self->metrics_.lock())
                    metrics->observe("protorelay_outbound_connect_duration_seconds", {},
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
                LOG_SMTP_INFO("Outbound: connected to {}", self->host_);
                self->send(); return;
            }
            if (result.outcome == smtp::Outcome::Rejected || result.outcome == smtp::Outcome::AuthFailed) {
                self->complete(std::move(result)); return;
            }
            if (!self->mx_pool_.empty() && ++self->mx_index_ < self->mx_pool_.size()) { self->connect(); return; }
            self->mx_index_ = 0;
            if (++self->retries_ >= 3) { self->complete(std::move(result)); return; }
            LOG_SMTP_WARN("Outbound: connection failed host={} retry={} reason={}",
                          self->host_, self->retries_, result.error);
            self->timer_.expires_after(std::chrono::milliseconds(500 * (1 << (self->retries_ - 1))));
            self->timer_.async_wait([self](auto ec) { if (!ec && !self->stopped_) self->connect(); });
        });
    });
}
void OutboundSmtpSession::send() {
    state_ = SessionState::DELIVERING;
    smtp::Message message{task_->sender, {task_->recipient}, payload_};
    auto self = shared_from_this();
    client_->async_send(std::move(message), [self](smtp::Result result) {
        asio::post(self->executor_, [self, result = std::move(result)]() mutable {
            if (self->stopped_) return;
            if (result.outcome == smtp::Outcome::Accepted) ++self->sent_;
            self->complete(std::move(result));
        });
    });
}
void OutboundSmtpSession::complete(smtp::Result result) {
    LOG_SMTP_INFO("Outbound: completed mail_id={} host={} stage={} code={} outcome={} reason={}",
                  task_->mail_id, host_, result.stage, result.code, static_cast<int>(result.outcome), result.error);
    const auto record_id = task_->record_id;
    task_.reset(); payload_.reset(); active_ = false;
    pending_.fetch_sub(1);
    if (complete_) complete_(record_id, std::move(result));
    if (sent_ >= 100 && client_ && client_->is_open()) { quit(); return; }
    // 不在协议错误回调栈里直接递归推进队列。
    asio::post(executor_, [self = shared_from_this()] { self->pump(); });
}
void OutboundSmtpSession::idle() {
    if (!client_ || !client_->is_open()) { state_ = SessionState::CLOSED; return; }
    state_ = SessionState::CONNECTED;
    timer_.expires_after(std::chrono::seconds(30));
    std::weak_ptr<OutboundSmtpSession> weak = shared_from_this();
    timer_.async_wait([weak](auto ec) {
        if (ec) return;
        if (auto self = weak.lock(); self && !self->stopped_ && !self->task_ &&
            self->queue_.empty() && self->state_ == SessionState::CONNECTED) self->quit();
    });
}
void OutboundSmtpSession::quit() {
    state_ = SessionState::CLOSING;
    auto self = shared_from_this();
    client_->async_quit([self](smtp::Result) {
        asio::post(self->executor_, [self] {
            if (self->stopped_) return;
            self->state_ = SessionState::CLOSED;
            self->client_.reset();
            self->pump();
        });
    });
}
void OutboundSmtpSession::close() {
    asio::post(executor_, [self = shared_from_this()] {
        self->stopped_ = true;
        self->timer_.cancel();
        if (self->client_) self->client_->close();
        const size_t owned = self->queue_.size() + (self->task_ ? 1 : 0);
        self->task_.reset(); self->payload_.reset();
        while (!self->queue_.empty()) self->queue_.pop();
        self->pending_.fetch_sub(owned);
        // 尚未执行的 submit post 自己递减，不能在这里清零 pending_。
        self->active_ = false;
        self->state_ = SessionState::CLOSED;
    });
}
} // namespace mail_system::outbound
