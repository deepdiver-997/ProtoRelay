#pragma once

#include "mail_system/back/webmail/session_password.h"
#include <boost/asio/any_io_executor.hpp>
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

// 固定 127.0.0.1，无 DNS/任意 host 配置；明文 AUTH 仅跨本机 loopback。
// 采用 outbound SMTP 的逐响应推进形状，共用 base64 / DATA transparency。
// socket/timer 统一在独立 strand 上发起，worker/HTTP 调用线程不碰 I/O 对象。
class LoopbackSmtpSubmitter final : public ISmtpSubmitter {
public:
    explicit LoopbackSmtpSubmitter(boost::asio::any_io_executor executor,
                                  uint16_t port = 587,
                                  std::chrono::milliseconds timeout = std::chrono::seconds(30));
    void submit_async(SubmitRequest request, Complete complete) override;
private:
    boost::asio::any_io_executor executor_;
    uint16_t port_;
    std::chrono::milliseconds timeout_;
};

} // namespace mail_system::webmail
