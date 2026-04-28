#ifndef PERSISTENT_QUEUE_H
#define PERSISTENT_QUEUE_H

#include "mail_system/back/entities/mail.h"
#include "mail_system/back/db/db_pool.h"
#include "mail_system/back/db/mysql_pool.h"
#include "mail_system/back/outbound/outbox_repository.h"
#include "mail_system/back/storage/i_storage_provider.h"
#include "mail_system/back/thread_pool/thread_pool_base.h"
#include "mail_system/back/common/logger.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mail_system {
namespace outbound {
class SmtpOutboundClient;
}
}

namespace mail_system {
namespace persist_storage {

struct PersistSubmissionTicket {
    std::uint64_t mail_id{0};
    mail_system::persist_storage::SharedPersistStatus status{};
    std::shared_ptr<std::atomic<bool>> cancel_requested{std::make_shared<std::atomic<bool>>(false)};

    bool valid() const {
        return mail_id != 0;
    }

    void request_cancel() const {
        if (cancel_requested) {
            cancel_requested->store(true, std::memory_order_release);
        }
    }

    bool is_cancel_requested() const {
        return cancel_requested && cancel_requested->load(std::memory_order_acquire);
    }
};

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
        std::shared_ptr<DBPool> db_pool,
        std::shared_ptr<ThreadPoolBase> worker_pool,
        std::shared_ptr<mail_system::storage::IStorageProvider> storage_provider = nullptr
    );

    ~PersistentQueue();

    SubmitOwnedMailResult submit_owned_mail(std::unique_ptr<mail> mail_data);
    
    // 提交多个邮件到持久化队列
    bool submit_mails(std::vector<mail*>& mail_list);

    void delete_task(mail* mail_data);

    void delete_multi_tasks(std::vector<mail*>& mail_list);

    // 获取队列大小
    size_t queue_size() const;

    // 获取当前在途邮件数（用于负载门控）
    size_t inflight_count() const {
        return inflight_mail_count_.load(std::memory_order_relaxed);
    }

    void set_outbound_client(std::shared_ptr<mail_system::outbound::SmtpOutboundClient> outbound_client);
    void set_local_domain(std::string local_domain);
    void set_batch_pop_size(size_t batch_size);
    void set_storage_provider(std::shared_ptr<mail_system::storage::IStorageProvider> storage_provider);
    void set_pressure_config(PersistentQueuePressureConfig config);

    // 关闭队列，等待所有任务完成
    void shutdown();

    // 检查是否已关闭
    bool is_shutdown() const { return shutdown_.load(std::memory_order_acquire); }

// private:
    // 处理单个持久化任务
    bool process_task();

    // 批量插入邮件元数据到数据库（一次连接，一条SQL）
    bool batch_insert_metadata(mail* mail_data, std::string& error);
    bool batch_insert_metadata(mail* mail_data, MySQLConnection* conn, std::string& error);

#if ENABLE_INBOUND_DEDUP_CHECK
    bool is_probable_duplicate_mail(mail* mail_data, MySQLConnection* conn);
#endif

    bool is_duplicate_by_source_message_id(mail* mail_data, MySQLConnection* conn);

    bool batch_delete_metadata(mail* mail_data, std::string& error);

    // 批量插入附件元数据到数据库
    bool batch_insert_attachments(mail* mail_data, std::string& error);
    bool batch_insert_attachments(mail* mail_data, MySQLConnection* conn, std::string& error);

    // 
    bool batch_delete_attachments(mail* mail_data, std::string& error);

    // 保存邮件正文到文件
    // bool save_mail_body(mail* mail_data, const std::string& file_path, std::string& error);

    // 保存附件到文件
    // bool save_attachments(mail* mail_data, std::string& error);

    // 主工作循环
    void worker_loop();

    bool enqueue_outbox_tasks(const mail& mail_data);
    bool enqueue_outbox_tasks(mail* mail_data,
                              MySQLConnection* conn,
                              const std::string& reserve_owner,
                              int reserve_lease_seconds,
                              std::vector<outbound::OutboxRecord>* reserved_records,
                              std::string& error);
    bool persist_mail_transactional(mail* mail_data,
                                    const std::string& reserve_owner,
                                    int reserve_lease_seconds,
                                    std::vector<outbound::OutboxRecord>* reserved_records,
                                    std::string& error);
    void cleanup_mail_files(mail* mail_data);
    void cleanup_failed_mail(mail* mail_data);
    bool should_reject_submission(const mail& mail_data, std::string& reason);
    bool is_db_under_pressure(std::string& reason) const;
    bool is_memory_under_pressure(std::string& reason) const;

    std::shared_ptr<DBPool> db_pool_;
    std::shared_ptr<ThreadPoolBase> worker_pool_;
    std::shared_ptr<mail_system::storage::IStorageProvider> storage_provider_;
    std::shared_ptr<mail_system::outbound::SmtpOutboundClient> outbound_client_;
    std::string local_domain_{"example.com"};
    PersistentQueuePressureConfig pressure_config_{};
    
    std::deque<std::pair<std::unique_ptr<mail>, PersistSubmissionTicket>> task_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::atomic<size_t> queued_task_count_{0};
    std::atomic<size_t> inflight_mail_count_{0};
    std::atomic<size_t> batch_pop_size_{16};
    
    std::atomic<bool> shutdown_;
    std::thread worker_thread_;
};

} // namespace persist_storage
} // namespace mail_system

#endif // PERSISTENT_QUEUE_H
