#pragma once

#include "mail_system/back/webmail/session_password.h"
#include "mail_system/back/smtp_client/client_session.h"
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mail_system::webmail {

struct SubmitRequest {
    std::string sender;
    SessionPasswordPtr password;
    std::vector<std::string> recipients;
    std::string raw;
    uint64_t user_id = 0;
    std::string subject;
    std::string message_id;
};

enum class SubmitStatus { Accepted, AuthFailed, Rejected, Unavailable, ProtocolError, OutcomeUnknown };

struct SubmitResult {
    SubmitStatus status = SubmitStatus::Unavailable;
    int smtp_code = 0;
    std::string stage;
};

class ISmtpSubmitter {
public:
    using Complete = std::function<void(SubmitResult)>;
    virtual ~ISmtpSubmitter() = default;
    virtual void submit_async(SubmitRequest request, Complete complete) = 0;
};

// 固定 loopback 提交策略；连接/协议由公共 SMTP 内核负责。
class LoopbackSmtpSubmitter final : public ISmtpSubmitter {
public:
    explicit LoopbackSmtpSubmitter(smtp::TransportProvider transport_provider,
                                  uint16_t port = 587,
                                  std::chrono::milliseconds timeout = std::chrono::seconds(30));
    void submit_async(SubmitRequest request, Complete complete) override;
private:
    smtp::TransportProvider transport_provider_;
    uint16_t port_;
    std::chrono::milliseconds timeout_;
};

} // namespace mail_system::webmail
