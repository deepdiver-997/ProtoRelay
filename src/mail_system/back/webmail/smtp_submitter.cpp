#include "mail_system/back/webmail/smtp_submitter.h"
#include "mail_system/back/common/logger.h"
#include "mail_system/back/common/mime_composer.h"
#include <stdexcept>

namespace mail_system::webmail {
namespace {
SubmitResult adapt(smtp::Result result) {
    auto status = SubmitStatus::ProtocolError;
    switch (result.outcome) {
        case smtp::Outcome::Accepted: status = SubmitStatus::Accepted; break;
        case smtp::Outcome::AuthFailed: status = SubmitStatus::AuthFailed; break;
        case smtp::Outcome::Rejected: status = SubmitStatus::Rejected; break;
        case smtp::Outcome::TemporaryFailure: case smtp::Outcome::Cancelled:
            status = SubmitStatus::Unavailable; break;
        case smtp::Outcome::OutcomeUnknown: status = SubmitStatus::OutcomeUnknown; break;
        default: break;
    }
    if (result.stage == "credentials") result.stage = "auth";
    return {status, result.code, std::move(result.stage)};
}
} // namespace

LoopbackSmtpSubmitter::LoopbackSmtpSubmitter(smtp::TransportProvider transport_provider,
                                           uint16_t port, std::chrono::milliseconds timeout)
    : transport_provider_(std::move(transport_provider)), port_(port), timeout_(timeout) {
    if (!transport_provider_) throw std::invalid_argument("SMTP submitter requires a transport provider");
}

void LoopbackSmtpSubmitter::submit_async(SubmitRequest request, Complete complete) {
    // 业务边界仍要求真实邮箱/会话凭据；内核也为 null reverse-path 等场景服务。
    bool valid = mime::valid_address(request.sender) && request.password && !request.raw.empty() &&
                 !request.recipients.empty() && request.recipients.size() <= mime::kMaxRecipients;
    for (const auto& rcpt : request.recipients) valid = valid && mime::valid_address(rcpt);
    if (!valid) {
        LOG_AT("WEBAPI", warn, "submission rejected: invalid sender, recipient or missing credentials");
        complete({SubmitStatus::ProtocolError, 0, "validation"}); return;
    }
    // provider 每次提交只取一次，socket/timer 后续固定在同一个 strand。
    auto client = std::make_shared<smtp::ClientSession>(transport_provider_());
    smtp::ConnectOptions options;
    options.host = "127.0.0.1";
    options.port = port_;
    options.helo = "webmail.local";
    options.auth_user = request.sender;
    options.password = std::move(request.password);
    options.timeout = timeout_;
    smtp::Message message{std::move(request.sender), std::move(request.recipients),
                         std::make_shared<const std::string>(std::move(request.raw)), timeout_};
    const auto deadline = std::chrono::steady_clock::now() + timeout_;
    client->async_connect(std::move(options),
        [client, deadline, message = std::move(message), complete = std::move(complete)](smtp::Result result) mutable {
            if (result.outcome != smtp::Outcome::Ready) { complete(adapt(std::move(result))); return; }
            message.timeout = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (message.timeout.count() <= 0) {
                LOG_AT("WEBAPI", warn, "submission deadline reached after authentication");
                client->close(); complete({SubmitStatus::Unavailable, 0, "connect"}); return;
            }
            client->async_send(std::move(message), [client, complete = std::move(complete)](smtp::Result result) mutable {
                if (result.outcome == smtp::Outcome::Accepted) {
                    // 250 已确认接受，HTTP 可立即完成；QUIT 失败不改变已接受的结果。
                    client->async_quit([client](smtp::Result) {});
                }
                complete(adapt(std::move(result)));
            });
        });
}
} // namespace mail_system::webmail
