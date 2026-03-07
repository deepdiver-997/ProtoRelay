#include "mail_system/back/outbound/smtp_outbound_client.h"

#include "mail_system/back/common/logger.h"
#include "mail_system/back/thread_pool/io_thread_pool.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <iostream>
#include <sstream>

namespace mail_system {
namespace outbound {

namespace {
constexpr std::size_t kClaimBatchSize = 32;
constexpr int kLeaseSeconds = 45;
constexpr int kLoopWaitMs = 200;
constexpr int kSmtpIoTimeoutSeconds = 10;
constexpr int kDefaultRetryDelaySeconds = 30;
std::atomic<std::uint64_t> g_dispatch_attempt_seq{0};

enum class SmtpStep {
    Connect,
    ReadGreeting,
    SendEhlo,
    SendMailFrom,
    SendRcptTo,
    SendData,
    SendBody,
    SendQuit,
    Done,
};

const char* smtp_step_name(SmtpStep step) {
    switch (step) {
    case SmtpStep::Connect:
        return "Connect";
    case SmtpStep::ReadGreeting:
        return "ReadGreeting";
    case SmtpStep::SendEhlo:
        return "SendEhlo";
    case SmtpStep::SendMailFrom:
        return "SendMailFrom";
    case SmtpStep::SendRcptTo:
        return "SendRcptTo";
    case SmtpStep::SendData:
        return "SendData";
    case SmtpStep::SendBody:
        return "SendBody";
    case SmtpStep::SendQuit:
        return "SendQuit";
    case SmtpStep::Done:
        return "Done";
    default:
        return "Unknown";
    }
}

struct SmtpExecResult {
    bool success{false};
    bool permanent_failure{false};
    int retry_delay_seconds{kDefaultRetryDelaySeconds};
    std::string response;
    std::string error_message;
};

std::string join_ports(const std::vector<std::uint16_t>& ports) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < ports.size(); ++i) {
        oss << ports[i];
        if (i + 1 < ports.size()) {
            oss << ",";
        }
    }
    return oss.str();
}

std::string trim_cr(std::string line) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

bool parse_smtp_code(const std::string& line, int& code_out) {
    if (line.size() < 3 ||
        !std::isdigit(static_cast<unsigned char>(line[0])) ||
        !std::isdigit(static_cast<unsigned char>(line[1])) ||
        !std::isdigit(static_cast<unsigned char>(line[2]))) {
        return false;
    }

    code_out = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
    return true;
}

bool read_smtp_response(boost::asio::ip::tcp::iostream& stream,
                        int& code_out,
                        std::string& response_out,
                        std::string& error_out) {
    response_out.clear();
    code_out = 0;

    std::string line;
    if (!std::getline(stream, line)) {
        error_out = "failed to read smtp response";
        return false;
    }

    line = trim_cr(line);
    response_out = line;

    if (!parse_smtp_code(line, code_out)) {
        error_out = "invalid smtp response: " + line;
        return false;
    }

    // Multi-line response: 250-... until 250 ...
    while (line.size() >= 4 && line[3] == '-') {
        if (!std::getline(stream, line)) {
            error_out = "truncated multi-line smtp response";
            return false;
        }
        line = trim_cr(line);
        response_out += "\\n" + line;
    }

    return true;
}

bool send_smtp_line(boost::asio::ip::tcp::iostream& stream, const std::string& line) {
    stream << line << "\r\n" << std::flush;
    return static_cast<bool>(stream);
}

bool looks_like_ehlo_capability_response(const std::string& response) {
    if (response.rfind("250-", 0) != 0) {
        return false;
    }
    return response.find("SMTPUTF8") != std::string::npos ||
           response.find("SIZE") != std::string::npos ||
           response.find("8BITMIME") != std::string::npos ||
           response.find("STARTTLS") != std::string::npos;
}

SmtpExecResult run_plain_smtp_flow(const OutboxRecord& record,
                                   const std::vector<std::uint16_t>& outbound_ports) {
    SmtpExecResult result;
    if (outbound_ports.empty()) {
        result.error_message = "outbound_ports is empty";
        return result;
    }

    LOG_SERVER_INFO("Outbound SMTP start: outbox_id={}, mail_id={}, recipient={}, ports=[{}]",
                    record.id,
                    record.mail_id,
                    record.recipient,
                    join_ports(outbound_ports));

    boost::asio::ip::tcp::iostream stream;
    stream.expires_after(std::chrono::seconds(kSmtpIoTimeoutSeconds));

    std::uint16_t connected_port = 0;
    for (auto port : outbound_ports) {
        stream.clear();
        stream.connect("127.0.0.1", std::to_string(port));
        if (stream) {
            connected_port = port;
            LOG_SERVER_INFO("Outbound SMTP connected: outbox_id={}, port={}", record.id, connected_port);
            break;
        }
    }

    if (connected_port == 0) {
        result.error_message = "failed to connect all configured outbound ports";
        result.retry_delay_seconds = kDefaultRetryDelaySeconds;
        LOG_SERVER_WARN("Outbound SMTP connect failed: outbox_id={}, ports=[{}]",
                        record.id,
                        join_ports(outbound_ports));
        return result;
    }

    int code = 0;
    std::string response;
    std::string error;
    SmtpStep step = SmtpStep::ReadGreeting;

    while (step != SmtpStep::Done) {
        stream.expires_after(std::chrono::seconds(kSmtpIoTimeoutSeconds));
        switch (step) {
        case SmtpStep::ReadGreeting:
            if (!read_smtp_response(stream, code, response, error)) {
                result.error_message = "read greeting failed: " + error;
                return result;
            }
            LOG_SERVER_INFO("Outbound SMTP step: outbox_id={}, step={}, code={}, response=[{}]",
                            record.id,
                            smtp_step_name(step),
                            code,
                            response);
            if (code != 220) {
                result.error_message = "unexpected greeting: " + response;
                result.permanent_failure = (code >= 500);
                return result;
            }
            step = SmtpStep::SendEhlo;
            break;

        case SmtpStep::SendEhlo:
            LOG_SERVER_INFO("Outbound SMTP step: outbox_id={}, step={}, request=EHLO outbound.local",
                            record.id,
                            smtp_step_name(step));
            if (!send_smtp_line(stream, "EHLO outbound.local")) {
                result.error_message = "send EHLO failed";
                return result;
            }
            if (!read_smtp_response(stream, code, response, error)) {
                result.error_message = "read EHLO response failed: " + error;
                return result;
            }
            LOG_SERVER_INFO("Outbound SMTP step: outbox_id={}, step={}, code={}, response=[{}]",
                            record.id,
                            smtp_step_name(step),
                            code,
                            response);
            if (code >= 400) {
                // Compatibility fallback: some servers reject EHLO but accept HELO.
                LOG_SERVER_WARN("Outbound SMTP EHLO rejected: outbox_id={}, code={}, response=[{}], trying HELO",
                                record.id,
                                code,
                                response);
                if (!send_smtp_line(stream, "HELO outbound.local")) {
                    result.error_message = "send HELO fallback failed";
                    return result;
                }
                if (!read_smtp_response(stream, code, response, error)) {
                    result.error_message = "read HELO fallback response failed: " + error;
                    return result;
                }
                LOG_SERVER_INFO("Outbound SMTP step: outbox_id={}, step=SendHeloFallback, code={}, response=[{}]",
                                record.id,
                                code,
                                response);
                if (code < 200 || code >= 400) {
                    result.error_message = "EHLO/HELO rejected: " + response;
                    result.permanent_failure = (code >= 500);
                    return result;
                }
            }
            step = SmtpStep::SendMailFrom;
            break;

        case SmtpStep::SendMailFrom: {
            LOG_SERVER_INFO("Outbound SMTP step: outbox_id={}, step={}, request=MAIL FROM:<{}>",
                            record.id,
                            smtp_step_name(step),
                            record.sender);
            if (!send_smtp_line(stream, "MAIL FROM:<" + record.sender + ">")) {
                result.error_message = "send MAIL FROM failed";
                return result;
            }
            if (!read_smtp_response(stream, code, response, error)) {
                result.error_message = "read MAIL FROM response failed: " + error;
                return result;
            }
            if (looks_like_ehlo_capability_response(response)) {
                LOG_SERVER_WARN("Outbound SMTP response realign: outbox_id={}, step=SendMailFrom, got EHLO capability response, reading next",
                                record.id);
                if (!read_smtp_response(stream, code, response, error)) {
                    result.error_message = "read MAIL FROM realign response failed: " + error;
                    return result;
                }
            }
            LOG_SERVER_INFO("Outbound SMTP step: outbox_id={}, step={}, code={}, response=[{}]",
                            record.id,
                            smtp_step_name(step),
                            code,
                            response);
            if (code < 200 || code >= 400) {
                result.error_message = "MAIL FROM rejected: " + response;
                result.permanent_failure = (code >= 500);
                return result;
            }
            step = SmtpStep::SendRcptTo;
            break;
        }

        case SmtpStep::SendRcptTo: {
            LOG_SERVER_INFO("Outbound SMTP step: outbox_id={}, step={}, request=RCPT TO:<{}>",
                            record.id,
                            smtp_step_name(step),
                            record.recipient);
            if (!send_smtp_line(stream, "RCPT TO:<" + record.recipient + ">")) {
                result.error_message = "send RCPT TO failed";
                return result;
            }
            if (!read_smtp_response(stream, code, response, error)) {
                result.error_message = "read RCPT TO response failed: " + error;
                return result;
            }
            LOG_SERVER_INFO("Outbound SMTP step: outbox_id={}, step={}, code={}, response=[{}]",
                            record.id,
                            smtp_step_name(step),
                            code,
                            response);
            if (code < 200 || code >= 400) {
                result.error_message = "RCPT TO rejected: " + response;
                result.permanent_failure = (code >= 500);
                return result;
            }
            step = SmtpStep::SendData;
            break;
        }

        case SmtpStep::SendData:
            LOG_SERVER_INFO("Outbound SMTP step: outbox_id={}, step={}, request=DATA",
                            record.id,
                            smtp_step_name(step));
            if (!send_smtp_line(stream, "DATA")) {
                result.error_message = "send DATA failed";
                return result;
            }
            if (!read_smtp_response(stream, code, response, error)) {
                result.error_message = "read DATA response failed: " + error;
                return result;
            }
            if (code == 250) {
                LOG_SERVER_WARN("Outbound SMTP response realign: outbox_id={}, step=SendData, got code=250 before 354, reading next",
                                record.id);
                std::string next_response;
                int next_code = 0;
                if (read_smtp_response(stream, next_code, next_response, error)) {
                    code = next_code;
                    response = std::move(next_response);
                }
            }
            LOG_SERVER_INFO("Outbound SMTP step: outbox_id={}, step={}, code={}, response=[{}]",
                            record.id,
                            smtp_step_name(step),
                            code,
                            response);
            if (code != 354) {
                result.error_message = "DATA rejected: " + response;
                result.permanent_failure = (code >= 500);
                return result;
            }
            step = SmtpStep::SendBody;
            break;

        case SmtpStep::SendBody: {
            std::ostringstream data;
            data << "Subject: Outbound relay test\r\n"
                 << "From: <" << record.sender << ">\r\n"
                 << "To: <" << record.recipient << ">\r\n"
                 << "\r\n"
                 << "relayed by outbound client, outbox_id=" << record.id << "\r\n"
                 << ".\r\n";
            stream << data.str() << std::flush;
            if (!stream) {
                result.error_message = "send message body failed";
                return result;
            }
            LOG_SERVER_INFO("Outbound SMTP step: outbox_id={}, step={}, request=<message body>",
                            record.id,
                            smtp_step_name(step));
            if (!read_smtp_response(stream, code, response, error)) {
                result.error_message = "read final DATA response failed: " + error;
                return result;
            }
            LOG_SERVER_INFO("Outbound SMTP step: outbox_id={}, step={}, code={}, response=[{}]",
                            record.id,
                            smtp_step_name(step),
                            code,
                            response);
            if (code < 200 || code >= 300) {
                result.error_message = "message rejected: " + response;
                result.permanent_failure = (code >= 500);
                return result;
            }
            result.response = response;
            step = SmtpStep::SendQuit;
            break;
        }

        case SmtpStep::SendQuit:
            LOG_SERVER_INFO("Outbound SMTP step: outbox_id={}, step={}, request=QUIT",
                            record.id,
                            smtp_step_name(step));
            send_smtp_line(stream, "QUIT");
            step = SmtpStep::Done;
            break;

        default:
            result.error_message = "unexpected smtp state";
            return result;
        }
    }

    result.success = true;
    if (result.response.empty()) {
        result.response = "250 message accepted";
    }
    LOG_SERVER_INFO("Outbound SMTP success: outbox_id={}, recipient={}, response={}",
                    record.id,
                    record.recipient,
                    result.response);
    return result;
}

std::string extract_domain(const std::string& email) {
    const auto at_pos = email.find('@');
    if (at_pos == std::string::npos || at_pos + 1 >= email.size()) {
        return {};
    }
    return email.substr(at_pos + 1);
}

bool has_external_recipient(const mail& mail_data, const std::string& local_domain) {
    for (const auto& recipient : mail_data.to) {
        const auto domain = extract_domain(recipient);
        if (!domain.empty() && domain != local_domain) {
            return true;
        }
    }
    return false;
}
}

SmtpOutboundClient::SmtpOutboundClient(std::shared_ptr<DBPool> db_pool,
                                       std::shared_ptr<ThreadPoolBase> io_thread_pool,
                                       std::shared_ptr<ThreadPoolBase> worker_thread_pool,
                                                                             std::string local_domain,
                                                                             std::vector<std::uint16_t> outbound_ports,
                                                                             int max_delivery_attempts)
    : db_pool_(std::move(db_pool)),
      io_thread_pool_(std::move(io_thread_pool)),
      worker_thread_pool_(std::move(worker_thread_pool)),
      repository_(db_pool_),
            local_domain_(std::move(local_domain)),
            outbound_ports_(std::move(outbound_ports)),
            max_delivery_attempts_(std::max(1, max_delivery_attempts)) {
        if (outbound_ports_.empty()) {
                outbound_ports_.push_back(25);
        }

    std::ostringstream oss;
    oss << "outbound-worker-" << std::this_thread::get_id();
    worker_id_ = oss.str();
}

SmtpOutboundClient::~SmtpOutboundClient() {
    stop();
}

void SmtpOutboundClient::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }

    orchestrator_thread_ = std::thread([this]() { run_loop(); });
    std::ostringstream ports;
    for (std::size_t i = 0; i < outbound_ports_.size(); ++i) {
        ports << outbound_ports_[i];
        if (i + 1 < outbound_ports_.size()) {
            ports << ",";
        }
    }
    LOG_SERVER_INFO("SmtpOutboundClient started, local_domain={}, outbound_ports=[{}], max_delivery_attempts={}",
                    local_domain_,
                    ports.str(),
                    max_delivery_attempts_);
}

void SmtpOutboundClient::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;
    }

    notify_cv_.notify_all();
    if (orchestrator_thread_.joinable()) {
        orchestrator_thread_.join();
    }
    LOG_SERVER_INFO("SmtpOutboundClient stopped");
}

bool SmtpOutboundClient::accept_mail_ownership(std::unique_ptr<mail> mail_ptr) {
    if (!mail_ptr) {
        return false;
    }

    if (!has_external_recipient(*mail_ptr, local_domain_)) {
        std::cout << "[OUTBOUND_FLOW_SKIP] mail_id=" << mail_ptr->id
                  << " reason=no-external-recipient local_domain=" << local_domain_
                  << std::endl;
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(notify_mutex_);
        ownership_queue_.push(std::move(mail_ptr));
        std::cout << "[OUTBOUND_FLOW_OK] Accepted mail ownership, mail_id=" << ownership_queue_.back()->id << std::endl;
    }
    notify_cv_.notify_one();
    return true;
}

void SmtpOutboundClient::notify_outbox_ready() {
    notify_cv_.notify_one();
}

void SmtpOutboundClient::run_loop() {
    while (running_.load()) {
        drain_notifications();
        drain_completion_queue();
        repository_.requeue_expired_leases();

        auto records = repository_.claim_batch(worker_id_, kClaimBatchSize, kLeaseSeconds);
        for (const auto& record : records) {
            dispatch_delivery_task(record);
        }

        std::unique_lock<std::mutex> lock(notify_mutex_);
        notify_cv_.wait_for(lock,
                            std::chrono::milliseconds(kLoopWaitMs),
                            [this]() {
                                return !running_.load() || !ownership_queue_.empty();
                            });
    }

    drain_notifications();
    drain_completion_queue();
}

void SmtpOutboundClient::drain_notifications() {
    std::queue<std::unique_ptr<mail>> local_queue;
    {
        std::lock_guard<std::mutex> lock(notify_mutex_);
        std::swap(local_queue, ownership_queue_);
    }

    while (!local_queue.empty()) {
        auto mail_ptr = std::move(local_queue.front());
        local_queue.pop();

        if (!mail_ptr) {
            continue;
        }
        hot_mail_cache_[mail_ptr->id] = std::move(mail_ptr);
    }
}

void SmtpOutboundClient::drain_completion_queue() {
    std::queue<DeliveryCompletion> local_queue;
    {
        std::lock_guard<std::mutex> lock(completion_mutex_);
        std::swap(local_queue, completion_queue_);
    }

    while (!local_queue.empty()) {
        auto completion = std::move(local_queue.front());
        local_queue.pop();

        bool ok = false;
        if (completion.success) {
            ok = repository_.mark_sent(completion.outbox_id, completion.smtp_response);
        } else if (completion.permanent_failure) {
            ok = repository_.mark_dead(completion.outbox_id, completion.error_message);
        } else {
            ok = repository_.mark_retry(completion.outbox_id,
                                       completion.error_message,
                                       completion.retry_delay_seconds);
        }

        if (!ok) {
            LOG_SERVER_ERROR("SmtpOutboundClient: failed to persist outbox state, outbox_id={}, attempt_id={}",
                             completion.outbox_id,
                             completion.dispatch_attempt_id);
        }

        // 保守策略：当前实现在完成一次状态回写后释放缓存对象。
        auto cache_it = hot_mail_cache_.find(completion.mail_id);
        if (cache_it != hot_mail_cache_.end()) {
            if (completion.success) {
                cache_it->second->status = 1;
            } else {
                cache_it->second->status = 2;
            }
            hot_mail_cache_.erase(cache_it);
        }
    }
}

void SmtpOutboundClient::dispatch_delivery_task(const OutboxRecord& record) {
    const auto attempt_id = ++g_dispatch_attempt_seq;

    auto task = [this, record, attempt_id]() mutable {
        std::cout << "[OUTBOUND_FLOW_OK] mail_id=" << record.mail_id
                  << " outbox_id=" << record.id
                  << " attempt_id=" << attempt_id
                  << " attempt_count=" << record.attempt_count
                  << std::endl;

        auto exec_result = run_plain_smtp_flow(record, outbound_ports_);

        DeliveryCompletion completion;
        completion.outbox_id = record.id;
        completion.mail_id = record.mail_id;
        completion.dispatch_attempt_id = attempt_id;
        completion.success = exec_result.success;
        completion.permanent_failure = exec_result.permanent_failure;
        completion.retry_delay_seconds = exec_result.retry_delay_seconds;
        completion.smtp_response = exec_result.response;
        completion.error_message = exec_result.error_message;

        if (!completion.success) {
            LOG_SERVER_WARN("Outbound SMTP failed: outbox_id={}, attempt_id={}, permanent_failure={}, error={}",
                            completion.outbox_id,
                            completion.dispatch_attempt_id,
                            completion.permanent_failure,
                            completion.error_message);
        }
        push_completion(std::move(completion));
    };

    auto io_pool = std::dynamic_pointer_cast<IOThreadPool>(io_thread_pool_);
    if (io_pool && io_pool->is_running()) {
        boost::asio::post(io_pool->get_io_context(), std::move(task));
        return;
    }

    if (worker_thread_pool_ && worker_thread_pool_->is_running()) {
        worker_thread_pool_->post(std::move(task));
        return;
    }

    DeliveryCompletion completion;
    completion.outbox_id = record.id;
    completion.mail_id = record.mail_id;
    completion.dispatch_attempt_id = attempt_id;
    completion.success = false;
    completion.error_message = "No available thread pool for outbound delivery";
    completion.permanent_failure = false;
    completion.retry_delay_seconds = 30;
    push_completion(std::move(completion));
}

void SmtpOutboundClient::push_completion(DeliveryCompletion completion) {
    {
        std::lock_guard<std::mutex> lock(completion_mutex_);
        completion_queue_.push(std::move(completion));
    }
    notify_cv_.notify_one();
}

} // namespace outbound
} // namespace mail_system
