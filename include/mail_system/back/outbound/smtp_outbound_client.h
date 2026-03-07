#ifndef MAIL_SYSTEM_SMTP_OUTBOUND_CLIENT_H
#define MAIL_SYSTEM_SMTP_OUTBOUND_CLIENT_H

#include "mail_system/back/outbound/outbox_repository.h"
#include "mail_system/back/thread_pool/thread_pool_base.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mail_system {
namespace outbound {

enum class DeliveryState : int {
    PENDING = 0,
    SUCCESS = 1,
    FAILED = 2,
};

struct DeliveryCompletion {
    std::uint64_t outbox_id{0};
    std::uint64_t mail_id{0};
    std::uint64_t dispatch_attempt_id{0};
    bool success{false};
    bool permanent_failure{false};
    int retry_delay_seconds{30};
    std::string smtp_response;
    std::string error_message;
};

class SmtpOutboundClient {
public:
    SmtpOutboundClient(std::shared_ptr<DBPool> db_pool,
                       std::shared_ptr<ThreadPoolBase> io_thread_pool,
                       std::shared_ptr<ThreadPoolBase> worker_thread_pool,
                       std::string local_domain,
                       std::vector<std::uint16_t> outbound_ports = {25, 587, 465},
                       int max_delivery_attempts = 8);

    ~SmtpOutboundClient();

    void start();
    void stop();

    bool accept_mail_ownership(std::unique_ptr<mail> mail_ptr);
    void notify_outbox_ready();

private:
    void run_loop();
    void drain_notifications();
    void drain_completion_queue();
    void dispatch_delivery_task(const OutboxRecord& record);
    void push_completion(DeliveryCompletion completion);

private:
    std::shared_ptr<DBPool> db_pool_;
    std::shared_ptr<ThreadPoolBase> io_thread_pool_;
    std::shared_ptr<ThreadPoolBase> worker_thread_pool_;
    OutboxRepository repository_;
    std::string local_domain_;
    std::vector<std::uint16_t> outbound_ports_;
    int max_delivery_attempts_{8};
    std::string worker_id_;

    std::thread orchestrator_thread_;
    std::atomic<bool> running_{false};

    std::mutex notify_mutex_;
    std::condition_variable notify_cv_;
    std::queue<std::unique_ptr<mail>> ownership_queue_;
    std::unordered_map<std::uint64_t, std::unique_ptr<mail>> hot_mail_cache_;

    std::mutex completion_mutex_;
    std::queue<DeliveryCompletion> completion_queue_;
};

} // namespace outbound
} // namespace mail_system

#endif // MAIL_SYSTEM_SMTP_OUTBOUND_CLIENT_H
