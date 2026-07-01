#ifndef MAIL_SYSTEM_OUTBOUND_SMTP_OUTBOUND_TRANSACTION_H
#define MAIL_SYSTEM_OUTBOUND_SMTP_OUTBOUND_TRANSACTION_H

#include "mail_system/back/common/logger.h"
#include "mail_system/back/outbound/outbound_utils.h"
#include "mail_system/back/outbound/outbox_repository.h"
#include "mail_system/back/outbound/smtp_transport_utils.h"

#include <boost/asio.hpp>
#include <string>

namespace mail_system {
namespace outbound {

enum class SmtpStep {
    Connect, ReadGreeting, SendEhlo, SendMailFrom,
    SendRcptTo, SendData, SendBody, SendQuit, Done,
};

inline const char* smtp_step_name(SmtpStep step) {
    switch (step) {
    case SmtpStep::ReadGreeting:   return "ReadGreeting";
    case SmtpStep::SendEhlo:       return "SendEhlo";
    case SmtpStep::SendMailFrom:   return "SendMailFrom";
    case SmtpStep::SendRcptTo:     return "SendRcptTo";
    case SmtpStep::SendData:       return "SendData";
    case SmtpStep::SendBody:       return "SendBody";
    case SmtpStep::SendQuit:       return "SendQuit";
    case SmtpStep::Done:           return "Done";
    default:                       return "Unknown";
    }
}

struct SmtpExecResult {
    bool success{false};
    bool permanent_failure{false};
    int retry_delay_seconds{30};
    std::string response;
    std::string error_message;
};

constexpr const char* kStartTlsReadyToken = "__STARTTLS_READY__";

using smtp_transport::ContinueFn;
namespace st = smtp_transport;

template <typename StreamType>
SmtpExecResult execute_smtp_transaction(
    StreamType& stream,
    boost::asio::streambuf& buffer,
    const OutboxRecord& record,
    const mail* hot_mail,
    const std::string& helo_domain,
    const std::string& envelope_sender,
    const std::string& header_from,
    const OutboundIdentityConfig& identity_config,
    bool allow_starttls_upgrade,
    bool expect_greeting,
    const ContinueFn& should_continue)
{
    SmtpExecResult result;
    int code = 0;
    std::string response, error;

    // Strip \r\n from addresses to prevent SMTP command injection
    auto strip_crlf = [](const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s)
            if (c != '\r' && c != '\n') out += c;
        return out;
    };
    const std::string safe_sender = strip_crlf(envelope_sender);
    const std::string safe_recipient = strip_crlf(record.recipient);

    SmtpStep step = expect_greeting ? SmtpStep::ReadGreeting : SmtpStep::SendEhlo;
    while (step != SmtpStep::Done) {
        if (should_continue && !should_continue()) {
            result.error_message = "outbound client stopping";
            return result;
        }

        switch (step) {
        case SmtpStep::ReadGreeting:
            if (!st::read_smtp_response(stream, buffer, code, response, should_continue, error))
                { result.error_message = "read greeting: " + error; return result; }
            if (code != 220)
                { result.error_message = "unexpected greeting: " + response; result.permanent_failure = (code >= 500); return result; }
            step = SmtpStep::SendEhlo;
            break;

        case SmtpStep::SendEhlo:
            if (!st::send_smtp_line(stream, "EHLO " + helo_domain, should_continue, error))
                { result.error_message = "send EHLO: " + error; return result; }
            if (!st::read_smtp_response(stream, buffer, code, response, should_continue, error))
                { result.error_message = "read EHLO: " + error; return result; }
            if (code >= 400) {
                if (!st::send_smtp_line(stream, "HELO " + helo_domain, should_continue, error))
                    { result.error_message = "send HELO: " + error; return result; }
                if (!st::read_smtp_response(stream, buffer, code, response, should_continue, error))
                    { result.error_message = "read HELO: " + error; return result; }
                if (code < 200 || code >= 400)
                    { result.error_message = "EHLO/HELO rejected: " + response; result.permanent_failure = (code >= 500); return result; }
            }
            if (allow_starttls_upgrade && response.find("STARTTLS") != std::string::npos) {
                if (!st::send_smtp_line(stream, "STARTTLS", should_continue, error))
                    { result.error_message = "send STARTTLS: " + error; return result; }
                if (!st::read_smtp_response(stream, buffer, code, response, should_continue, error))
                    { result.error_message = "read STARTTLS: " + error; return result; }
                if (code != 220)
                    { result.error_message = "STARTTLS rejected: " + response; result.permanent_failure = (code >= 500); return result; }
                result.error_message = kStartTlsReadyToken;
                return result;
            }
            step = SmtpStep::SendMailFrom;
            break;

        case SmtpStep::SendMailFrom:
            if (!st::send_smtp_line(stream, "MAIL FROM:<" + safe_sender + ">", should_continue, error))
                { result.error_message = "send MAIL FROM: " + error; return result; }
            if (!st::read_smtp_response(stream, buffer, code, response, should_continue, error))
                { result.error_message = "read MAIL FROM: " + error; return result; }
            if (st::looks_like_ehlo_capability_response(response))
                if (!st::read_smtp_response(stream, buffer, code, response, should_continue, error))
                    { result.error_message = "read MAIL FROM realign: " + error; return result; }
            if (code < 200 || code >= 400)
                { result.error_message = "MAIL FROM rejected: " + response; result.permanent_failure = (code >= 500); return result; }
            step = SmtpStep::SendRcptTo;
            break;

        case SmtpStep::SendRcptTo:
            if (!st::send_smtp_line(stream, "RCPT TO:<" + safe_recipient + ">", should_continue, error))
                { result.error_message = "send RCPT TO: " + error; return result; }
            if (!st::read_smtp_response(stream, buffer, code, response, should_continue, error))
                { result.error_message = "read RCPT TO: " + error; return result; }
            if (code < 200 || code >= 400)
                { result.error_message = "RCPT TO rejected: " + response; result.permanent_failure = (code >= 500); return result; }
            step = SmtpStep::SendData;
            break;

        case SmtpStep::SendData:
            if (!st::send_smtp_line(stream, "DATA", should_continue, error))
                { result.error_message = "send DATA: " + error; return result; }
            if (!st::read_smtp_response(stream, buffer, code, response, should_continue, error))
                { result.error_message = "read DATA: " + error; return result; }
            if (code == 250) {
                int next_code = 0; std::string next_response;
                if (st::read_smtp_response(stream, buffer, next_code, next_response, should_continue, error))
                    { code = next_code; response = std::move(next_response); }
            }
            if (code != 354)
                { result.error_message = "DATA rejected: " + response; result.permanent_failure = (code >= 500); return result; }
            step = SmtpStep::SendBody;
            break;

        case SmtpStep::SendBody: {
            bool dkim_applied = false; std::string dkim_error, message_id;
            const std::string wire_message = build_outbound_message(record,
                hot_mail, header_from, identity_config,
                &dkim_applied, &dkim_error, &message_id);
            if (!st::send_smtp_data(stream, wire_message, should_continue, error))
                { result.error_message = "send body: " + error; return result; }
            if (!st::read_smtp_response(stream, buffer, code, response, should_continue, error))
                { result.error_message = "read DATA final: " + error; return result; }
            if (code < 200 || code >= 300)
                { result.error_message = "message rejected: " + response; result.permanent_failure = (code >= 500); return result; }
            result.response = response;
            step = SmtpStep::SendQuit;
            break;
        }

        case SmtpStep::SendQuit:
            (void)st::send_smtp_line(stream, "QUIT", should_continue, error);
            step = SmtpStep::Done;
            break;

        default:
            result.error_message = "unexpected smtp state";
            return result;
        }
    }

    result.success = true;
    if (result.response.empty()) result.response = "250 message accepted";
    return result;
}

} // namespace outbound
} // namespace mail_system

#endif // MAIL_SYSTEM_OUTBOUND_SMTP_OUTBOUND_TRANSACTION_H
