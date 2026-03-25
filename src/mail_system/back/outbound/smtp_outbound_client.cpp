#include "mail_system/back/outbound/smtp_outbound_client.h"

#include "mail_system/back/outbound/cares_dns_resolver.h"
#include "mail_system/back/common/logger.h"
#include "mail_system/back/outbound/mx_routing_utils.h"
#include "mail_system/back/outbound/outbound_utils.h"
#include "mail_system/back/outbound/smtp_transport_utils.h"
#include "mail_system/back/thread_pool/io_thread_pool.h"

#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl.hpp>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

namespace mail_system {
namespace outbound {

namespace {
constexpr std::size_t kClaimBatchSize = 32;
constexpr int kLeaseSeconds = 45;
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

using ContinueFn = smtp_transport::ContinueFn;
using smtp_transport::kIoOperationTimeout;
using smtp_transport::looks_like_ehlo_capability_response;
using smtp_transport::read_smtp_response;
using smtp_transport::run_interruptible_ec;
using smtp_transport::send_smtp_data;
using smtp_transport::send_smtp_line;

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

std::chrono::milliseconds compute_adaptive_wait(const OutboundPollingConfig& cfg,
                                                std::size_t empty_claim_rounds,
                                                bool has_memory_pending) {
    const int busy_sleep_ms = std::max(1, cfg.busy_sleep_ms);
    const int backoff_base_ms = std::max(1, cfg.backoff_base_ms);
    const int backoff_max_ms = std::max(busy_sleep_ms, cfg.backoff_max_ms);
    const std::size_t backoff_shift_cap = cfg.backoff_shift_cap;

    if (has_memory_pending) {
        return std::chrono::milliseconds(busy_sleep_ms);
    }

    const std::size_t shift = std::min(empty_claim_rounds, backoff_shift_cap);
    int wait_ms = backoff_base_ms << shift;
    wait_ms = std::max(busy_sleep_ms, std::min(wait_ms, backoff_max_ms));
    return std::chrono::milliseconds(wait_ms);
}

std::string rewrite_sender_domain(const std::string& sender, const std::string& domain_override) {
    if (domain_override.empty()) {
        return sender;
    }
    const auto at_pos = sender.find('@');
    if (at_pos == std::string::npos) {
        return sender;
    }
    return sender.substr(0, at_pos + 1) + domain_override;
}

constexpr const char* kStartTlsReadyToken = "__STARTTLS_READY__";

template <typename StreamType>
SmtpExecResult execute_smtp_transaction(StreamType& stream,
                                        boost::asio::streambuf& buffer,
                                        const OutboxRecord& record,
                                        const std::string& helo_domain,
                                        const std::string& envelope_sender,
                                        const std::string& header_from,
                                        const OutboundIdentityConfig& identity_config,
                                        bool allow_starttls_upgrade,
                                        bool expect_greeting,
                                        const ContinueFn& should_continue) {
    SmtpExecResult result;
    int code = 0;
    std::string response;
    std::string error;

    SmtpStep step = expect_greeting ? SmtpStep::ReadGreeting : SmtpStep::SendEhlo;
    while (step != SmtpStep::Done) {
        if (should_continue && !should_continue()) {
            result.error_message = "outbound client stopping";
            return result;
        }

        switch (step) {
        case SmtpStep::ReadGreeting:
            if (!read_smtp_response(stream, buffer, code, response, should_continue, error)) {
                result.error_message = "read greeting failed: " + error;
                return result;
            }
            LOG_OUTBOUND_DEBUG("Outbound SMTP step: outbox_id={}, step={}, code={}, response=[{}]",
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
            LOG_OUTBOUND_DEBUG("Outbound SMTP step: outbox_id={}, step={}, request=EHLO {}",
                            record.id,
                            smtp_step_name(step),
                            helo_domain);
            if (!send_smtp_line(stream, "EHLO " + helo_domain, should_continue, error)) {
                result.error_message = "send EHLO failed: " + error;
                return result;
            }
            if (!read_smtp_response(stream, buffer, code, response, should_continue, error)) {
                result.error_message = "read EHLO response failed: " + error;
                return result;
            }
            LOG_OUTBOUND_DEBUG("Outbound SMTP step: outbox_id={}, step={}, code={}, response=[{}]",
                            record.id,
                            smtp_step_name(step),
                            code,
                            response);

            if (code >= 400) {
                LOG_OUTBOUND_WARN("Outbound SMTP EHLO rejected: outbox_id={}, code={}, response=[{}], trying HELO",
                                record.id,
                                code,
                                response);
                if (!send_smtp_line(stream, "HELO " + helo_domain, should_continue, error)) {
                    result.error_message = "send HELO fallback failed: " + error;
                    return result;
                }
                if (!read_smtp_response(stream, buffer, code, response, should_continue, error)) {
                    result.error_message = "read HELO fallback response failed: " + error;
                    return result;
                }
                LOG_OUTBOUND_DEBUG("Outbound SMTP step: outbox_id={}, step=SendHeloFallback, code={}, response=[{}]",
                                record.id,
                                code,
                                response);
                if (code < 200 || code >= 400) {
                    result.error_message = "EHLO/HELO rejected: " + response;
                    result.permanent_failure = (code >= 500);
                    return result;
                }
            }

            if (allow_starttls_upgrade && response.find("STARTTLS") != std::string::npos) {
                LOG_OUTBOUND_DEBUG("Outbound SMTP step: outbox_id={}, step=SendStartTls, request=STARTTLS",
                                record.id);
                if (!send_smtp_line(stream, "STARTTLS", should_continue, error)) {
                    result.error_message = "send STARTTLS failed: " + error;
                    return result;
                }
                if (!read_smtp_response(stream, buffer, code, response, should_continue, error)) {
                    result.error_message = "read STARTTLS response failed: " + error;
                    return result;
                }
                LOG_OUTBOUND_DEBUG("Outbound SMTP step: outbox_id={}, step=SendStartTls, code={}, response=[{}]",
                                record.id,
                                code,
                                response);
                if (code != 220) {
                    result.error_message = "STARTTLS rejected: " + response;
                    result.permanent_failure = (code >= 500);
                    return result;
                }

                result.error_message = kStartTlsReadyToken;
                return result;
            }

            step = SmtpStep::SendMailFrom;
            break;

        case SmtpStep::SendMailFrom:
            LOG_OUTBOUND_DEBUG("Outbound SMTP step: outbox_id={}, step={}, request=MAIL FROM:<{}>",
                            record.id,
                            smtp_step_name(step),
                            envelope_sender);
            if (!send_smtp_line(stream, "MAIL FROM:<" + envelope_sender + ">", should_continue, error)) {
                result.error_message = "send MAIL FROM failed: " + error;
                return result;
            }
            if (!read_smtp_response(stream, buffer, code, response, should_continue, error)) {
                result.error_message = "read MAIL FROM response failed: " + error;
                return result;
            }
            if (looks_like_ehlo_capability_response(response)) {
                LOG_OUTBOUND_WARN("Outbound SMTP response realign: outbox_id={}, step=SendMailFrom, got EHLO capability response, reading next",
                                record.id);
                if (!read_smtp_response(stream, buffer, code, response, should_continue, error)) {
                    result.error_message = "read MAIL FROM realign response failed: " + error;
                    return result;
                }
            }
            LOG_OUTBOUND_DEBUG("Outbound SMTP step: outbox_id={}, step={}, code={}, response=[{}]",
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

        case SmtpStep::SendRcptTo:
            LOG_OUTBOUND_DEBUG("Outbound SMTP step: outbox_id={}, step={}, request=RCPT TO:<{}>",
                            record.id,
                            smtp_step_name(step),
                            record.recipient);
            if (!send_smtp_line(stream, "RCPT TO:<" + record.recipient + ">", should_continue, error)) {
                result.error_message = "send RCPT TO failed: " + error;
                return result;
            }
            if (!read_smtp_response(stream, buffer, code, response, should_continue, error)) {
                result.error_message = "read RCPT TO response failed: " + error;
                return result;
            }
            LOG_OUTBOUND_DEBUG("Outbound SMTP step: outbox_id={}, step={}, code={}, response=[{}]",
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

        case SmtpStep::SendData:
            LOG_OUTBOUND_DEBUG("Outbound SMTP step: outbox_id={}, step={}, request=DATA",
                            record.id,
                            smtp_step_name(step));
            if (!send_smtp_line(stream, "DATA", should_continue, error)) {
                result.error_message = "send DATA failed: " + error;
                return result;
            }
            if (!read_smtp_response(stream, buffer, code, response, should_continue, error)) {
                result.error_message = "read DATA response failed: " + error;
                return result;
            }
            if (code == 250) {
                LOG_OUTBOUND_WARN("Outbound SMTP response realign: outbox_id={}, step=SendData, got code=250 before 354, reading next",
                                record.id);
                int next_code = 0;
                std::string next_response;
                if (read_smtp_response(stream, buffer, next_code, next_response, should_continue, error)) {
                    code = next_code;
                    response = std::move(next_response);
                }
            }
            LOG_OUTBOUND_DEBUG("Outbound SMTP step: outbox_id={}, step={}, code={}, response=[{}]",
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
            bool dkim_applied = false;
            std::string dkim_error;
            std::string message_id;
            const std::string wire_message = build_outbound_message(record,
                                                                    header_from,
                                                                    identity_config,
                                                                    &dkim_applied,
                                                                    &dkim_error,
                                                                    &message_id);
            if (identity_config.dkim_enabled && !dkim_applied && !dkim_error.empty()) {
                LOG_OUTBOUND_WARN("Outbound DKIM disabled for this message: outbox_id={}, reason={}",
                                record.id,
                                dkim_error);
            }
            if (!send_smtp_data(stream, wire_message, should_continue, error)) {
                result.error_message = "send message body failed: " + error;
                return result;
            }
            LOG_OUTBOUND_DEBUG("Outbound SMTP step: outbox_id={}, step={}, message_id={}, request=<message body>",
                            record.id,
                            smtp_step_name(step),
                            message_id);
            if (!read_smtp_response(stream, buffer, code, response, should_continue, error)) {
                result.error_message = "read final DATA response failed: " + error;
                return result;
            }
            LOG_OUTBOUND_DEBUG("Outbound SMTP step: outbox_id={}, step={}, code={}, response=[{}]",
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
            LOG_OUTBOUND_DEBUG("Outbound SMTP step: outbox_id={}, step={}, request=QUIT",
                            record.id,
                            smtp_step_name(step));
            (void)send_smtp_line(stream, "QUIT", should_continue, error);
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
    return result;
}

SmtpExecResult run_plain_smtp_flow(const OutboxRecord& record,
                                   const std::vector<std::uint16_t>& outbound_ports,
                                   const OutboundIdentityConfig& identity_config,
                                   const std::string& target_host,
                                   const ContinueFn& should_continue) {
    SmtpExecResult result;
    if (should_continue && !should_continue()) {
        result.error_message = "outbound client stopping";
        return result;
    }

    if (outbound_ports.empty()) {
        result.error_message = "outbound_ports is empty";
        return result;
    }

    LOG_OUTBOUND_DEBUG("Outbound SMTP start: outbox_id={}, mail_id={}, recipient={}, target_host={}, ports=[{}]",
                     record.id,
                     record.mail_id,
                     record.recipient,
                     target_host,
                     join_ports(outbound_ports));

    const std::string helo_domain = identity_config.helo_domain.empty() ? "outbound.local" : identity_config.helo_domain;
    const std::string envelope_sender = rewrite_sender_domain(record.sender, identity_config.mail_from_domain);
    const std::string header_from = identity_config.rewrite_header_from ? envelope_sender : record.sender;

    boost::asio::io_context io_context;
    boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tls_client);
    ssl_ctx.set_default_verify_paths();
    ssl_ctx.set_verify_mode(boost::asio::ssl::verify_none);

    boost::system::error_code addr_ec;
    auto target_ip = boost::asio::ip::make_address(target_host, addr_ec);
    if (addr_ec) {
        result.error_message = "invalid target address: " + target_host;
        return result;
    }

    std::uint16_t connected_port = 0;
    for (auto port : outbound_ports) {
        if (should_continue && !should_continue()) {
            result.error_message = "outbound client stopping";
            break;
        }

        boost::asio::ip::tcp::socket socket(io_context);
        std::string connect_error;
        if (!run_interruptible_ec(
                socket,
                [&](auto handler) { socket.async_connect(boost::asio::ip::tcp::endpoint(target_ip, port), std::move(handler)); },
                should_continue,
                kIoOperationTimeout,
                "smtp connect",
                connect_error)) {
            if (connect_error.find("interrupted") != std::string::npos) {
                result.error_message = "outbound client stopping";
                break;
            }
            continue;
        }

        connected_port = port;
        LOG_OUTBOUND_INFO("Outbound SMTP connected: outbox_id={}, host={}, port={}",
                        record.id,
                        target_host,
                        connected_port);

        if (port == 465) {
            boost::asio::ssl::stream<boost::asio::ip::tcp::socket> tls_stream(std::move(socket), ssl_ctx);
            std::string handshake_error;
            if (!run_interruptible_ec(
                    tls_stream,
                    [&](auto handler) { tls_stream.async_handshake(boost::asio::ssl::stream_base::client, std::move(handler)); },
                    should_continue,
                    kIoOperationTimeout,
                    "implicit TLS handshake",
                    handshake_error)) {
                if (handshake_error.find("interrupted") != std::string::npos) {
                    result.success = false;
                    result.error_message = "outbound client stopping";
                    break;
                }
                result.success = false;
                result.error_message = handshake_error;
                continue;
            }
            LOG_OUTBOUND_INFO("Outbound SMTP TLS handshake succeeded: outbox_id={}, host={}, port={}",
                            record.id,
                            target_host,
                            port);

            boost::asio::streambuf tls_buffer;
            result = execute_smtp_transaction(tls_stream,
                                              tls_buffer,
                                              record,
                                              helo_domain,
                                              envelope_sender,
                                              header_from,
                                              identity_config,
                                              false,
                                              true,
                                              should_continue);
        } else {
            boost::asio::streambuf plain_buffer;
            result = execute_smtp_transaction(socket,
                                              plain_buffer,
                                              record,
                                              helo_domain,
                                              envelope_sender,
                                              header_from,
                                              identity_config,
                                              true,
                                              true,
                                              should_continue);
            if (result.error_message == kStartTlsReadyToken) {
                if (should_continue && !should_continue()) {
                    result.success = false;
                    result.error_message = "outbound client stopping";
                    break;
                }

                boost::asio::ssl::stream<boost::asio::ip::tcp::socket> tls_stream(std::move(socket), ssl_ctx);
                std::string handshake_error;
                if (!run_interruptible_ec(
                        tls_stream,
                        [&](auto handler) { tls_stream.async_handshake(boost::asio::ssl::stream_base::client, std::move(handler)); },
                        should_continue,
                        kIoOperationTimeout,
                        "STARTTLS handshake",
                        handshake_error)) {
                    if (handshake_error.find("interrupted") != std::string::npos) {
                        result.success = false;
                        result.error_message = "outbound client stopping";
                        break;
                    }
                    result.success = false;
                    result.error_message = handshake_error;
                    continue;
                }
                LOG_OUTBOUND_INFO("Outbound SMTP TLS handshake succeeded: outbox_id={}, host={}, port={}",
                                record.id,
                                target_host,
                                port);

                boost::asio::streambuf tls_buffer;
                result = execute_smtp_transaction(tls_stream,
                                                  tls_buffer,
                                                  record,
                                                  helo_domain,
                                                  envelope_sender,
                                                  header_from,
                                                  identity_config,
                                                  false,
                                                  false,
                                                  should_continue);
            }
        }

        if (result.success || result.permanent_failure) {
            break;
        }
    }

    if (connected_port == 0) {
        result.error_message = "failed to connect all configured outbound ports";
        result.retry_delay_seconds = kDefaultRetryDelaySeconds;
        LOG_OUTBOUND_WARN("Outbound SMTP connect failed: outbox_id={}, host={}, ports=[{}]",
                        record.id,
                        target_host,
                        join_ports(outbound_ports));
        return result;
    }

    if (result.success) {
        LOG_OUTBOUND_INFO("Outbound SMTP success: outbox_id={}, recipient={}, response={}",
                        record.id,
                        record.recipient,
                        result.response);
    }
    return result;
}

}

SmtpOutboundClient::SmtpOutboundClient(std::shared_ptr<DBPool> db_pool,
                                       std::shared_ptr<ThreadPoolBase> io_thread_pool,
                                       std::shared_ptr<ThreadPoolBase> worker_thread_pool,
                                       std::shared_ptr<IDnsResolver> dns_resolver,
                                       std::shared_ptr<std::atomic<bool>> server_interrupt_flag,
                                       OutboundIdentityConfig identity_config,
                                       OutboundPollingConfig polling_config,
                                                                             std::string local_domain,
                                                                             std::vector<std::uint16_t> outbound_ports,
                                                                             int max_delivery_attempts)
    : db_pool_(std::move(db_pool)),
      io_thread_pool_(std::move(io_thread_pool)),
      worker_thread_pool_(std::move(worker_thread_pool)),
      dns_resolver_(std::move(dns_resolver)),
            server_interrupt_flag_(std::move(server_interrupt_flag)),
            identity_config_(std::move(identity_config)),
        polling_config_(std::move(polling_config)),
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

    polling_config_.busy_sleep_ms = std::max(1, polling_config_.busy_sleep_ms);
    polling_config_.backoff_base_ms = std::max(1, polling_config_.backoff_base_ms);
    polling_config_.backoff_max_ms = std::max(polling_config_.busy_sleep_ms, polling_config_.backoff_max_ms);

    if (identity_config_.dkim_enabled) {
        LOG_OUTBOUND_INFO("DKIM config loaded: selector={}, domain={}, key_file={}",
                          identity_config_.dkim_selector,
                          identity_config_.dkim_domain,
                          identity_config_.dkim_private_key_file);
    }
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
    LOG_OUTBOUND_INFO("SmtpOutboundClient started, local_domain={}, outbound_ports=[{}], max_delivery_attempts={}",
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
    LOG_OUTBOUND_INFO("SmtpOutboundClient stopped");
}

bool SmtpOutboundClient::accept_mail_ownership(std::unique_ptr<mail> mail_ptr) {
    if (!mail_ptr) {
        return false;
    }

    if (!has_external_recipient(*mail_ptr, local_domain_)) {
        LOG_OUTBOUND_DEBUG("Outbound flow skip: mail_id={}, reason=no-external-recipient, local_domain={}",
                         mail_ptr->id,
                         local_domain_);
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(notify_mutex_);
        ownership_queue_.push(std::move(mail_ptr));
        LOG_OUTBOUND_DEBUG("Outbound flow accepted ownership: mail_id={}", ownership_queue_.back()->id);
    }
    notify_cv_.notify_one();
    return true;
}

void SmtpOutboundClient::notify_outbox_ready() {
    notify_cv_.notify_one();
}

void SmtpOutboundClient::run_loop() {
    std::size_t empty_claim_rounds = 0;

    while (running_.load()) {
        drain_notifications();
        drain_completion_queue();
        repository_.requeue_expired_leases();

        auto records = repository_.claim_batch(worker_id_, kClaimBatchSize, kLeaseSeconds);
        if (!records.empty()) {
            empty_claim_rounds = 0;
        } else {
            ++empty_claim_rounds;
        }

        for (const auto& record : records) {
            dispatch_delivery_task(record);
        }

        std::unique_lock<std::mutex> lock(notify_mutex_);
        const auto wait_duration = compute_adaptive_wait(polling_config_, empty_claim_rounds, !ownership_queue_.empty());
        notify_cv_.wait_for(lock,
                            wait_duration,
                            [this]() {
                                if (!running_.load() || !ownership_queue_.empty()) {
                                    return true;
                                }
                                std::lock_guard<std::mutex> completion_lock(completion_mutex_);
                                return !completion_queue_.empty();
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
            LOG_OUTBOUND_ERROR("SmtpOutboundClient: failed to persist outbox state, outbox_id={}, attempt_id={}",
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
        const auto should_continue = [this]() {
            if (!running_.load()) {
                return false;
            }
            return !server_interrupt_flag_ || server_interrupt_flag_->load();
        };

        if (!should_continue()) {
            DeliveryCompletion completion;
            completion.outbox_id = record.id;
            completion.mail_id = record.mail_id;
            completion.dispatch_attempt_id = attempt_id;
            completion.success = false;
            completion.permanent_failure = false;
            completion.retry_delay_seconds = 5;
            completion.error_message = "outbound client stopping";
            push_completion(std::move(completion));
            return;
        }

        LOG_OUTBOUND_DEBUG("Outbound flow dispatch: mail_id={}, outbox_id={}, attempt_id={}, attempt_count={}",
                 record.mail_id,
                 record.id,
                 attempt_id,
                 record.attempt_count);

        // Resolve MX targets for recipient domain before SMTP delivery.
        auto target_hosts = build_target_hosts(record, dns_resolver_.get());
        LOG_OUTBOUND_DEBUG("Outbound DNS targets: outbox_id={}, recipient={}, hosts=[{}]",
                         record.id,
                         record.recipient,
                         [&target_hosts]() {
                             std::ostringstream oss;
                             for (std::size_t i = 0; i < target_hosts.size(); ++i) {
                                 oss << target_hosts[i];
                                 if (i + 1 < target_hosts.size()) {
                                     oss << ",";
                                 }
                             }
                             return oss.str();
                         }());

        if (target_hosts.empty()) {
            DeliveryCompletion completion;
            completion.outbox_id = record.id;
            completion.mail_id = record.mail_id;
            completion.dispatch_attempt_id = attempt_id;
            completion.success = false;
            completion.permanent_failure = false;
            completion.retry_delay_seconds = kDefaultRetryDelaySeconds;
            completion.error_message = "no routable SMTP target hosts resolved";
            LOG_OUTBOUND_WARN("Outbound SMTP failed: outbox_id={}, attempt_id={}, permanent_failure={}, error={}",
                            completion.outbox_id,
                            completion.dispatch_attempt_id,
                            completion.permanent_failure,
                            completion.error_message);
            push_completion(std::move(completion));
            return;
        }

        SmtpExecResult exec_result;
        bool delivered = false;
        for (const auto& host : target_hosts) {
            if (!should_continue()) {
                exec_result.success = false;
                exec_result.permanent_failure = false;
                exec_result.error_message = "outbound client stopping";
                break;
            }

            exec_result = run_plain_smtp_flow(record, outbound_ports_, identity_config_, host, should_continue);
            if (exec_result.success) {
                delivered = true;
                break;
            }
            if (exec_result.permanent_failure) {
                break;
            }
        }

        if (!delivered && exec_result.success) {
            exec_result.success = false;
            exec_result.error_message = "delivery status inconsistent";
        }

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
            LOG_OUTBOUND_WARN("Outbound SMTP failed: outbox_id={}, attempt_id={}, permanent_failure={}, error={}",
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
