#ifndef PERSISTENT_QUEUE_H
#define PERSISTENT_QUEUE_H

#include "mail_system/back/entities/mail.h"
#include "mail_system/back/persist_storage/persist_submission.h"
#include <condition_variable>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <utility>

namespace pr { class MetricsServer; class ThreadPoolBase; }
namespace mail_system::router { class IShardRouter; }
namespace mail_system::outbound { class OutboundServer; }
namespace mail_system::repository { class IPersistenceRepository; }
namespace mail_system { using pr::MetricsServer; }

namespace mail_system {
namespace persist_storage {

struct SubmitOwnedMailResult {
    bool accepted{false};
    PersistSubmissionTicket ticket{};
    std::unique_ptr<mail> rejected_mail{};
    std::string error{};
};

struct PersistentQueuePressureConfig {
    std::size_t max_inflight_mails{2048};
    std::size_t min_available_memory_mb{256};
    std::size_t min_db_available_connections{1};
};

class PersistentQueue {
public:
    PersistentQueue(
        std::shared_ptr<router::IShardRouter> shard_router,
        std::shared_ptr<pr::ThreadPoolBase> worker_pool,
        std::shared_ptr<repository::IPersistenceRepository> repository = {}
    );

    ~PersistentQueue();

    // ---- 公开接口 ----
    SubmitOwnedMailResult submit_owned_mail(std::unique_ptr<mail> mail_data);
    bool submit_mails(std::vector<mail*>& mail_list);
    void delete_task(mail* mail_data);
    void delete_multi_tasks(std::vector<mail*>& mail_list);

    size_t queue_size() const;
    size_t inflight_count() const {
        return inflight_mail_count_.load(std::memory_order_relaxed);
    }

    void set_outbound_server(std::shared_ptr<mail_system::outbound::OutboundServer> server);
    void set_local_domain(std::string local_domain);
    void set_batch_pop_size(size_t batch_size);
    void set_pressure_config(PersistentQueuePressureConfig config);
    // 出站投递最大尝试次数：写进 mail_outbox.max_attempts 行（供 claim/重试/DEAD 判断）
    void set_max_attempts(size_t max_attempts);
    void set_persistence_repository(std::shared_ptr<repository::IPersistenceRepository> repository);

    // 停止接收，取消尚未开始的任务；析构在 worker/DB 运行时仍存活时排空在途回调。
    void shutdown();
    bool is_shutdown() const { return shutdown_.load(std::memory_order_acquire); }

    void inject_metrics(std::weak_ptr<pr::MetricsServer> m) { metrics_ = std::move(m); }

    // 持久化回调: void(bool success, std::string error)
    using PersistCallback = std::function<void(bool, std::string)>;

private:
    void push_queue_metrics();
    void finish_task();
    // ---- 工作线程 ----
    void worker_loop();
    bool process_task();

    // ---- 清理 ----
    void cleanup_mail_files(mail* mail_data);
    void cleanup_failed_mail(mail* mail_data, const std::shared_ptr<repository::IPersistenceRepository>& repository);

    // ---- 背压检查 ----
    bool should_reject_submission(const mail& mail_data, std::string& reason);
    bool is_db_under_pressure(std::string& reason) const;
    bool is_memory_under_pressure(std::string& reason) const;

    // ---- 分片辅助 ----
    int shard_from_mail(const mail* m) const;

    // ---- 成员变量 ----
    std::shared_ptr<router::IShardRouter> m_shardRouter;
    std::shared_ptr<pr::ThreadPoolBase> worker_pool_;
    std::shared_ptr<repository::IPersistenceRepository> repository_;
    std::shared_ptr<mail_system::outbound::OutboundServer> outbound_server_;
    std::string local_domain_{"example.com"};
    size_t max_attempts_{8};               // outbox 行 max_attempts（默认对齐 kDefaultMaxAttempts）
    PersistentQueuePressureConfig pressure_config_{};

    // The fixed-capacity lockfree implementation is private to the .cpp.
    struct QueueItem;
    struct TaskQueue;
    std::unique_ptr<TaskQueue> task_queue_;
    std::atomic<size_t> queued_task_count_{0};
    std::mutex submission_mutex_;
    std::condition_variable available_;
    std::condition_variable drained_;
    std::atomic<size_t> inflight_mail_count_{0};
    std::atomic<size_t> batch_pop_size_{16};

    std::atomic<bool> shutdown_;
    std::thread worker_thread_;
    std::weak_ptr<pr::MetricsServer> metrics_;
};

} // namespace persist_storage
} // namespace mail_system

#endif // PERSISTENT_QUEUE_H
