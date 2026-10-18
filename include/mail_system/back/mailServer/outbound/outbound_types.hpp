#ifndef OUTBOUND_TYPES_HPP
#define OUTBOUND_TYPES_HPP

#include "mail_system/back/entities/mail.h"
#include <chrono>
#include <cstdint>
#include <string>
#include <memory>

namespace mail_system {
namespace outbound {

// ================================================================
// Session 生命周期状态（独立于 SMTP 协议状态机）
// ================================================================
enum class SessionState {
    INIT,          // 初始，尚未连接
    CONNECTING,    // DNS/connect 进行中
    CONNECTED,     // 已连接，空闲等待任务
    DELIVERING,    // 正在投递中
    CLOSING,       // 优雅关闭中 (QUIT)
    CLOSED         // 连接已终止
};

// ================================================================
// 投递任务
// ================================================================
struct MailDeliveryTask {
    using Clock = std::chrono::steady_clock;

    uint64_t mail_id;
    uint64_t record_id;
    std::string sender;
    std::string recipient;
    std::shared_ptr<struct mail> mail_ptr;
    int attempt_count = 0;
    int max_attempts = 8;
    Clock::time_point created_at = Clock::now();
    std::chrono::seconds ttl = std::chrono::seconds(300);  // 5 分钟过期

    bool expired() const { return Clock::now() - created_at > ttl; }
};

} // namespace outbound
} // namespace mail_system

#endif // OUTBOUND_TYPES_HPP
