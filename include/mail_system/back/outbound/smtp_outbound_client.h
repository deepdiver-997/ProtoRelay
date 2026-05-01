#ifndef MAIL_SYSTEM_SMTP_OUTBOUND_CLIENT_H
#define MAIL_SYSTEM_SMTP_OUTBOUND_CLIENT_H

#include "mail_system/back/outbound/dns_resolver.h"
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

struct OutboundIdentityConfig {
    std::string helo_domain{"outbound.local"};
    std::string mail_from_domain{};
    bool rewrite_header_from{true};

    // DKIM 配置入口（当前先透传，后续可接入签名实现）。
    bool dkim_enabled{false};
    std::string dkim_selector{"default"};
    std::string dkim_domain{};
    std::string dkim_private_key_file{};
};

struct OutboundPollingConfig {
    int busy_sleep_ms{20};
    int backoff_base_ms{50};
    int backoff_max_ms{1200};
    std::size_t backoff_shift_cap{6};
};

struct HotMailDispatch {
    std::unique_ptr<mail> mail_ptr;
    std::vector<OutboxRecord> reserved_records;
};

struct HotDeliveryTask {
    OutboxRecord record;
    std::shared_ptr<mail> mail_ctx;
};

class SmtpOutboundClient {
public:
    SmtpOutboundClient(std::shared_ptr<DBPool> db_pool,
                       std::shared_ptr<ThreadPoolBase> io_thread_pool,
                       std::shared_ptr<ThreadPoolBase> worker_thread_pool,
                       std::shared_ptr<IDnsResolver> dns_resolver,
                       std::shared_ptr<std::atomic<bool>> server_interrupt_flag,
                       OutboundIdentityConfig identity_config,
                       OutboundPollingConfig polling_config,
                       std::string local_domain,
                       std::vector<std::uint16_t> outbound_ports = {25, 587, 465},
                       int max_delivery_attempts = 8);

    ~SmtpOutboundClient();

    void start();
    void stop();

    bool accept_mail_ownership(std::unique_ptr<mail> mail_ptr);
    bool accept_reserved_mail_ownership(std::unique_ptr<mail> mail_ptr,
                                        std::vector<OutboxRecord> reserved_records);
    void notify_outbox_ready();
    const std::string& worker_id() const { return worker_id_; }
    int local_reservation_lease_seconds() const;
    std::shared_ptr<IDnsResolver> get_dns_resolver() const { return dns_resolver_; }

private:
    void run_loop();
    void drain_notifications();
    std::size_t drain_hot_records();
    void schedule_claimed_records(std::vector<OutboxRecord> claimed_records);
    void drain_completion_queue();
    void dispatch_delivery_task(const OutboxRecord& record,
                                std::shared_ptr<mail> hot_mail_ctx = nullptr);
    void push_completion(DeliveryCompletion completion);
    bool try_enqueue_hot_dispatch(std::unique_ptr<mail>& mail_ptr,
                                  std::vector<OutboxRecord>& reserved_records);

private:
    std::shared_ptr<DBPool> db_pool_;
    std::shared_ptr<ThreadPoolBase> io_thread_pool_;
    std::shared_ptr<ThreadPoolBase> worker_thread_pool_;
    std::shared_ptr<IDnsResolver> dns_resolver_;
    std::shared_ptr<std::atomic<bool>> server_interrupt_flag_;
    OutboundIdentityConfig identity_config_;
    OutboundPollingConfig polling_config_;
    OutboxRepository repository_;
    std::string local_domain_;
    std::vector<std::uint16_t> outbound_ports_;
    int max_delivery_attempts_{8};
    std::string worker_id_;

    std::thread orchestrator_thread_;
    std::atomic<bool> running_{false};

    std::mutex notify_mutex_;
    std::condition_variable notify_cv_;
    std::queue<HotMailDispatch> ownership_queue_;
    std::queue<HotDeliveryTask> hot_record_queue_;
    std::unordered_map<std::uint64_t, std::shared_ptr<mail>> hot_mail_cache_;
    std::unordered_map<std::uint64_t, std::size_t> hot_mail_pending_dispatch_counts_;

    std::mutex completion_mutex_;
    std::queue<DeliveryCompletion> completion_queue_;
};

} // namespace outbound
} // namespace mail_system

#endif // MAIL_SYSTEM_SMTP_OUTBOUND_CLIENT_H
