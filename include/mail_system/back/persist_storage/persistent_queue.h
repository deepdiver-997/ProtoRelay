#ifndef PERSISTENT_QUEUE_H
#define PERSISTENT_QUEUE_H

#include "mail_system/back/entities/mail.h"
#include "mail_system/back/db/db_pool.h"
#include "mail_system/back/db/mysql_pool.h"
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

class PersistentQueue {
public:
    PersistentQueue(
        std::shared_ptr<DBPool> db_pool,
        std::shared_ptr<ThreadPoolBase> worker_pool
    );

    ~PersistentQueue();

    // 提交邮件到持久化队列
    bool submit_mail(mail* mail_data);
    
    // 提交多个邮件到持久化队列
    bool submit_mails(std::vector<mail*>& mail_list);

    void delete_task(mail* mail_data);

    void delete_multi_tasks(std::vector<mail*>& mail_list);

    // 获取队列大小
    size_t queue_size() const;

    void set_outbound_client(std::shared_ptr<mail_system::outbound::SmtpOutboundClient> outbound_client);
    void set_local_domain(std::string local_domain);
    void set_batch_pop_size(size_t batch_size);

    // 关闭队列，等待所有任务完成
    void shutdown();

    // 检查是否已关闭
    bool is_shutdown() const { return shutdown_.load(std::memory_order_acquire); }

// private:
    // 处理单个持久化任务
    bool process_task();

    // 批量插入邮件元数据到数据库（一次连接，一条SQL）
    bool batch_insert_metadata(mail* mail_data, std::string& error);

#if ENABLE_INBOUND_DEDUP_CHECK
    bool is_probable_duplicate_mail(mail* mail_data, MySQLConnection* conn);
#endif

    bool batch_delete_metadata(mail* mail_data, std::string& error);

    // 批量插入附件元数据到数据库
    bool batch_insert_attachments(mail* mail_data, std::string& error);

    // 
    bool batch_delete_attachments(mail* mail_data, std::string& error);

    // 保存邮件正文到文件
    // bool save_mail_body(mail* mail_data, const std::string& file_path, std::string& error);

    // 保存附件到文件
    // bool save_attachments(mail* mail_data, std::string& error);

    // 主工作循环
    void worker_loop();

    void enqueue_outbox_tasks(mail* mail_data);

    std::shared_ptr<DBPool> db_pool_;
    std::shared_ptr<ThreadPoolBase> worker_pool_;
    std::shared_ptr<mail_system::outbound::SmtpOutboundClient> outbound_client_;
    std::string local_domain_{"example.com"};
    
    std::deque<mail*> task_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    size_t MAX_TASK_COUNT = 100;
    std::atomic<size_t> current_task_count_{0};
    std::atomic<size_t> batch_pop_size_{16};
    
    std::atomic<bool> shutdown_;
    std::thread worker_thread_;
};

} // namespace persist_storage
} // namespace mail_system

#endif // PERSISTENT_QUEUE_H
