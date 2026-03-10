#include "mail_system/back/persist_storage/persistent_queue.h"
#include "mail_system/back/algorithm/snow.h"
#include "mail_system/back/algorithm/smtp_utils.h"
#include "mail_system/back/outbound/outbox_repository.h"
#include "mail_system/back/outbound/smtp_outbound_client.h"
#include <array>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <unordered_map>

namespace mail_system {
namespace persist_storage {

#if ENABLE_INBOUND_DEDUP_CHECK
namespace {
constexpr int kInboundDedupWindowSeconds = 600;
std::mutex g_inbound_dedup_mu;
std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_inbound_dedup_seen;
}
#endif

namespace {

constexpr size_t kDefaultBatchPopSize = 16;
constexpr size_t kMaxBatchPopSize = 256;

size_t adjust_batch_size_by_backlog(size_t current_batch, size_t backlog_size) {
    const size_t safe_current = std::max<size_t>(1, std::min(current_batch, kMaxBatchPopSize));

    // Grow quickly when backlog is much larger than current pull size.
    if (backlog_size > safe_current * 4) {
        return std::min(kMaxBatchPopSize, safe_current * 2);
    }

    // Shrink when load drops, but keep at least default size under normal operation.
    if (backlog_size < safe_current / 2 && safe_current > kDefaultBatchPopSize) {
        return std::max(kDefaultBatchPopSize, safe_current / 2);
    }

    // Under very light load, allow a small batch to reduce per-iteration latency.
    if (backlog_size <= 2 && safe_current > 4) {
        return safe_current / 2;
    }

    return safe_current;
}

std::string extract_domain_lower(const std::string& email) {
    const auto at_pos = email.find('@');
    if (at_pos == std::string::npos || at_pos + 1 >= email.size()) {
        return {};
    }
    std::string domain = email.substr(at_pos + 1);
    std::transform(domain.begin(), domain.end(), domain.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return domain;
}

int recipient_status_for_domain(const std::string& recipient,
                                const std::string& local_domain) {
    const auto recipient_domain = extract_domain_lower(recipient);
    const auto system_domain = extract_domain_lower(local_domain);
    // 1=未读（本域），2=未送达（外域）
    return (!recipient_domain.empty() && !system_domain.empty() && recipient_domain == system_domain) ? 1 : 2;
}

} // namespace

PersistentQueue::PersistentQueue(
    std::shared_ptr<DBPool> db_pool,
    std::shared_ptr<ThreadPoolBase> worker_pool
)
    : db_pool_(db_pool),
      worker_pool_(worker_pool),
      current_task_count_(0),
      batch_pop_size_(kDefaultBatchPopSize),
      shutdown_(false) {
    if (const char* env_batch = std::getenv("PERSIST_QUEUE_BATCH_POP_SIZE"); env_batch != nullptr) {
        const auto parsed = std::strtoul(env_batch, nullptr, 10);
        if (parsed > 0) {
            set_batch_pop_size(std::min(static_cast<size_t>(parsed), kMaxBatchPopSize));
        }
    }

    // 启动工作线程
    worker_thread_ = std::thread(&PersistentQueue::worker_loop, this);
    LOG_PERSISTENT_QUEUE_INFO(
        "PersistentQueue initialized and worker thread started, batch_pop_size={}",
        batch_pop_size_.load(std::memory_order_relaxed));
}

PersistentQueue::~PersistentQueue() {
    shutdown();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

bool PersistentQueue::submit_mail(mail* mail_data) {
    if (is_shutdown()) {
        LOG_PERSISTENT_QUEUE_ERROR("Cannot submit mail, PersistentQueue is shutdown");
        return false;
    }

    if (current_task_count_.load(std::memory_order_relaxed) >= MAX_TASK_COUNT) {
        LOG_PERSISTENT_QUEUE_WARN("PersistentQueue is full, cannot accept new mail tasks");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        // auto it = std::lower_bound(
        //     task_queue_.begin(),
        //     task_queue_.end(),
        //     mail_data,
        //     [](mail* a, mail* b) { return a->id < b->id; }
        // );
        // task_queue_.insert(it, mail_data);
        task_queue_.push_back(mail_data);
        current_task_count_.fetch_add(1, std::memory_order_relaxed);
    }
    // 唤醒等待中的工作线程处理新任务
    queue_cv_.notify_one();
    LOG_PERSISTENT_QUEUE_INFO("Mail submitted to PersistentQueue, current queue size: {}", queue_size());
    return true;
}

bool PersistentQueue::submit_mails(std::vector<mail*>& mail_list) {
    if (is_shutdown()) {
        LOG_PERSISTENT_QUEUE_ERROR("Cannot submit mails, PersistentQueue is shutdown");
        return false;
    }
    
    if (current_task_count_.load(std::memory_order_relaxed) + mail_list.size() > MAX_TASK_COUNT) {
        LOG_PERSISTENT_QUEUE_WARN("PersistentQueue is full, cannot accept new mail tasks");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        // for (auto& mail_data : mail_list) {
        //     auto it = std::lower_bound(
        //         task_queue_.begin(),
        //         task_queue_.end(),
        //         mail_data,
        //         [](mail* a, mail* b) { return a->id < b->id; }
        //     );
        //     task_queue_.insert(it, mail_data);
        task_queue_.insert(task_queue_.end(), mail_list.begin(), mail_list.end());
        current_task_count_.fetch_add(mail_list.size(), std::memory_order_relaxed);
    }
    queue_cv_.notify_all();
    LOG_PERSISTENT_QUEUE_INFO("Mails submitted to PersistentQueue, current queue size: {}", queue_size());
    return true;
}

void PersistentQueue::delete_task(mail* mail_data) {
    if (!mail_data) {
        LOG_PERSISTENT_QUEUE_ERROR("Cannot delete task: mail_data is null");
        return;
    }

    auto mysql_pool = std::dynamic_pointer_cast<MySQLPool>(db_pool_);
    auto connection = mysql_pool->get_raii_connection();
    auto conn = connection.get();
    if (!conn || !conn->is_connected()) {
        LOG_PERSISTENT_QUEUE_ERROR("Failed to get database connection for deleting mail ID {}", mail_data->id);
        return;
    }

    try {
        // 第一步：删除邮件收发件人关系记录
        std::string recipient_sql = "DELETE FROM mail_recipients WHERE mail_id = " + std::to_string(mail_data->id);
        if (!conn->execute(recipient_sql)) {
            LOG_PERSISTENT_QUEUE_WARN("Failed to delete mail recipients for mail ID {}", mail_data->id);
        }

        // 第二步：删除邮件元数据（会级联删除附件）
        std::string mail_sql = "DELETE FROM mails WHERE id = " + std::to_string(mail_data->id);
        if (!conn->execute(mail_sql)) {
            LOG_PERSISTENT_QUEUE_WARN("Failed to delete mail metadata for mail ID {}", mail_data->id);
        } else {
            LOG_PERSISTENT_QUEUE_INFO("Successfully deleted mail ID {}", mail_data->id);
        }
    } catch (const std::exception& e) {
        LOG_PERSISTENT_QUEUE_ERROR("Exception during delete_task for mail ID {}: {}", 
                                  mail_data->id, e.what());
    }
    // RAIIConnection 析构时自动释放连接
}

void PersistentQueue::delete_multi_tasks(std::vector<mail*>& mail_list) {
    if (mail_list.empty()) {
        return;
    }

    auto mysql_pool = std::dynamic_pointer_cast<MySQLPool>(db_pool_);
    auto connection = mysql_pool->get_raii_connection();
    auto conn = connection.get();
    if (!conn || !conn->is_connected()) {
        LOG_PERSISTENT_QUEUE_ERROR("Failed to get database connection for batch delete");
        return;
    }

    try {
        // 构建批量删除SQL - 收集所有要删除的邮件ID
        std::string id_list;
        for (size_t i = 0; i < mail_list.size(); ++i) {
            if (mail_list[i]) {
                id_list += std::to_string(mail_list[i]->id);
                if (i < mail_list.size() - 1) {
                    id_list += ",";
                }
            }
        }

        if (!id_list.empty()) {
            // 第一步：删除邮件收发件人关系记录
            std::string recipient_sql = "DELETE FROM mail_recipients WHERE mail_id IN (" + id_list + ")";
            if (!conn->execute(recipient_sql)) {
                LOG_PERSISTENT_QUEUE_WARN("Failed to batch delete mail recipients");
            }

            // 第二步：删除邮件元数据（会级联删除附件）
            std::string mail_sql = "DELETE FROM mails WHERE id IN (" + id_list + ")";
            if (!conn->execute(mail_sql)) {
                LOG_PERSISTENT_QUEUE_WARN("Failed to batch delete mail metadata");
            } else {
                LOG_PERSISTENT_QUEUE_INFO("Successfully batch deleted {} mail records", mail_list.size());
            }
        }
    } catch (const std::exception& e) {
        LOG_PERSISTENT_QUEUE_ERROR("Exception during delete_multi_tasks: {}", e.what());
    }
    // RAIIConnection 析构时自动释放连接
}

size_t PersistentQueue::queue_size() const {
    return current_task_count_.load(std::memory_order_relaxed);
}

void PersistentQueue::set_outbound_client(std::shared_ptr<mail_system::outbound::SmtpOutboundClient> outbound_client) {
    outbound_client_ = std::move(outbound_client);
}

void PersistentQueue::set_local_domain(std::string local_domain) {
    if (!local_domain.empty()) {
        local_domain_ = std::move(local_domain);
    }
}

void PersistentQueue::set_batch_pop_size(size_t batch_size) {
    const size_t clamped = std::max<size_t>(1, std::min(batch_size, kMaxBatchPopSize));
    batch_pop_size_.store(clamped, std::memory_order_relaxed);
}

void PersistentQueue::shutdown() {
    if (shutdown_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    // 唤醒可能在等待中的工作线程以便正常退出
    queue_cv_.notify_all();
    // 不在这里打印日志，因为可能在析构阶段调用，此时 Logger 可能已 shutdown
}

bool PersistentQueue::process_task() {
    std::vector<mail*> pending_batch;

    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_cv_.wait(lock, [this] {
            return shutdown_.load(std::memory_order_relaxed) || !task_queue_.empty();
        });
        if (shutdown_.load(std::memory_order_relaxed) && task_queue_.empty()) {
            return false;
        }

        const size_t backlog_size = task_queue_.size();
        const size_t current_batch = batch_pop_size_.load(std::memory_order_relaxed);
        const size_t adaptive_batch = adjust_batch_size_by_backlog(current_batch, backlog_size);
        if (adaptive_batch != current_batch) {
            batch_pop_size_.store(adaptive_batch, std::memory_order_relaxed);
        }

        const size_t pop_count = std::min(backlog_size, adaptive_batch);
        pending_batch.reserve(pop_count);
        for (size_t i = 0; i < pop_count; ++i) {
            pending_batch.push_back(task_queue_.front());
            task_queue_.pop_front();
        }
        current_task_count_.fetch_sub(pending_batch.size(), std::memory_order_relaxed);
    }

    for (auto* mail_data : pending_batch) {
        if (!mail_data) {
            continue;
        }

        if (mail_data->persist_status != PersistStatus::PENDING) {
            LOG_PERSISTENT_QUEUE_WARN("Mail ID {} has already been processed with status {}, skipping", mail_data->id, static_cast<int>(mail_data->persist_status));
            continue;
        }

        worker_pool_->post([this, mail_data]() {
            std::string error;
            bool success = true;

            mail_data->persist_status = PersistStatus::PROCESSING;

            if (!batch_insert_attachments(mail_data, error)) {
                LOG_PERSISTENT_QUEUE_ERROR("Failed to insert attachments for mail ID {}: {}", mail_data->id, error);
                success = false;
            }

            if (!batch_insert_metadata(mail_data, error)) {
                LOG_PERSISTENT_QUEUE_ERROR("Failed to insert metadata for mail ID {}: {}", mail_data->id, error);
                success = false;
            }

            if (success) {
                mail_data->persist_status = PersistStatus::SUCCESS;
                if (!mail_data->deduplicated_inbound) {
                    enqueue_outbox_tasks(mail_data);
                } else {
                    LOG_PERSISTENT_QUEUE_INFO("Mail {} deduplicated before insert, skipped outbox enqueue", mail_data->id);
                }
                LOG_PERSISTENT_QUEUE_INFO("Successfully processed mail ID {}", mail_data->id);
            } else {
                mail_data->persist_status = PersistStatus::FAILED;
                LOG_PERSISTENT_QUEUE_ERROR("Processing failed for mail ID {}", mail_data->id);
            }
        });
    }

    return true;
}

bool PersistentQueue::batch_insert_metadata(mail* mail_data, std::string& error) {
    if (!mail_data) {
        error = "Mail data is null";
        return false;
    }

    auto mysql_pool = std::dynamic_pointer_cast<MySQLPool>(db_pool_);
    auto connection = mysql_pool->get_raii_connection();
    auto conn = std::dynamic_pointer_cast<MySQLConnection>(connection.get());
    if (!conn || !conn->is_connected()) {
        error = "Failed to get database connection";
        LOG_PERSISTENT_QUEUE_ERROR("Failed to get database connection for mail ID {}", mail_data->id);
        return false;
    }

    try {
#if ENABLE_INBOUND_DEDUP_CHECK
        if (is_probable_duplicate_mail(mail_data, conn.get())) {
            mail_data->deduplicated_inbound = true;
            LOG_PERSISTENT_QUEUE_INFO("Dedup hit before DB insert, mail_id={}, message_id=[{}]",
                                     mail_data->id,
                                     mail_data->source_message_id);
            return true;
        }
#endif

        // 第一步：插入邮件元数据到 mails 表
        std::string mail_sql = "INSERT INTO mails (id, subject, body_path) VALUES (" +
                              std::to_string(mail_data->id) + ", '" +
                              conn->escape_string(mail_data->subject) + "', '" +
                              conn->escape_string(mail_data->body_path) + "'" + ")";
        
        if (!conn->execute(mail_sql)) {
            error = "Failed to insert mail metadata";
            LOG_PERSISTENT_QUEUE_ERROR("Failed to insert mail metadata with sql: {}", mail_sql);
            return false;
        }

        // 第二步：插入邮件收发件人关系到 mail_recipients 表
        // 一份邮件可能有多个收件人，所以为每个收件人插入一条关系记录
        std::string recipient_sql = "INSERT INTO mail_recipients (id, mail_id, sender, recipient, status) VALUES";
        for (size_t i = 0; i < mail_data->to.size(); ++i) {
            recipient_sql += " (" + std::to_string(mail_data->ids[i]) + ", " +
                            std::to_string(mail_data->id) + ", '" +
                            conn->escape_string(mail_data->from) + "', '" +
                            conn->escape_string(mail_data->to[i]) +
                            "', " + std::to_string(recipient_status_for_domain(mail_data->to[i], local_domain_)) + "),";
        }
        recipient_sql[recipient_sql.size() - 1] = ';'; // 替换最后一个逗号为分号
        
        if (!conn->execute(recipient_sql)) {
            error = "Failed to insert mail recipients";
            LOG_PERSISTENT_QUEUE_ERROR("Failed to insert mail recipients with sql: {}", recipient_sql);
            // 如果插入收件人失败，删除已插入的邮件元数据
            std::string delete_sql = "DELETE FROM mails WHERE id = " + std::to_string(mail_data->id);
            conn->execute(delete_sql);
            return false;
        }

        LOG_PERSISTENT_QUEUE_INFO("Successfully inserted metadata for mail ID {} with {} recipients", 
                                 mail_data->id, mail_data->to.size());
        return true;
    } catch (const std::exception& e) {
        error = std::string("Exception during metadata insertion: ") + e.what();
        LOG_PERSISTENT_QUEUE_ERROR("Exception during metadata insertion for mail ID {}: {}", 
                                  mail_data->id, e.what());
        return false;
    }
    // RAIIConnection 析构时自动释放连接
}

#if ENABLE_INBOUND_DEDUP_CHECK
bool PersistentQueue::is_probable_duplicate_mail(mail* mail_data, MySQLConnection* conn) {
    if (!mail_data || !conn || mail_data->to.empty()) {
        return false;
    }

    const std::string sender = conn->escape_string(mail_data->from);
    const std::string subject = conn->escape_string(mail_data->subject);

    // First priority: upstream Message-ID in a short-lived in-memory cache.
    if (!mail_data->source_message_id.empty()) {
        const auto now = std::chrono::steady_clock::now();
        const auto ttl = std::chrono::seconds(kInboundDedupWindowSeconds);
        std::lock_guard<std::mutex> lock(g_inbound_dedup_mu);

        for (auto it = g_inbound_dedup_seen.begin(); it != g_inbound_dedup_seen.end();) {
            if (now - it->second > ttl) {
                it = g_inbound_dedup_seen.erase(it);
            } else {
                ++it;
            }
        }

        bool all_seen = true;
        for (const auto& recipient_raw : mail_data->to) {
            const std::string key = mail_data->from + "\n" + recipient_raw + "\n" + mail_data->source_message_id;
            if (g_inbound_dedup_seen.find(key) == g_inbound_dedup_seen.end()) {
                all_seen = false;
            }
        }
        if (all_seen) {
            return true;
        }

        for (const auto& recipient_raw : mail_data->to) {
            const std::string key = mail_data->from + "\n" + recipient_raw + "\n" + mail_data->source_message_id;
            g_inbound_dedup_seen[key] = now;
        }

        // Also run DB heuristic below so restart/cross-process duplicates are still reduced.
        std::string sql =
            "SELECT id FROM mails WHERE subject = '" + subject + "' "
            "AND id IN (SELECT mail_id FROM mail_recipients WHERE sender = '" + sender + "') "
            "AND TIMESTAMPDIFF(SECOND, send_time, NOW()) BETWEEN 0 AND " + std::to_string(kInboundDedupWindowSeconds) + " "
            "LIMIT 20";

        auto result = conn->query(sql);
        if (result && result->get_row_count() > 0) {
            // Message-ID is not persisted as a dedicated DB column yet; use it as a strict in-memory hint
            // together with sender/subject/time window and recipient matching below.
            for (const auto& recipient_raw : mail_data->to) {
                const std::string recipient = conn->escape_string(recipient_raw);
                std::string rcpt_sql =
                    "SELECT 1 FROM mail_recipients r "
                    "JOIN mails m ON m.id = r.mail_id "
                    "WHERE r.sender='" + sender + "' "
                    "AND r.recipient='" + recipient + "' "
                    "AND m.subject='" + subject + "' "
                    "AND TIMESTAMPDIFF(SECOND, m.send_time, NOW()) BETWEEN 0 AND " + std::to_string(kInboundDedupWindowSeconds) + " "
                    "LIMIT 1";
                auto rcpt_res = conn->query(rcpt_sql);
                if (!(rcpt_res && rcpt_res->get_row_count() > 0)) {
                    return false;
                }
            }
            return true;
        }
    }

    // Fallback heuristic when Message-ID is missing: sender+subject+all recipients in a short window.
    if (mail_data->subject.empty()) {
        return false;
    }

    for (const auto& recipient_raw : mail_data->to) {
        const std::string recipient = conn->escape_string(recipient_raw);
        std::string sql =
            "SELECT 1 FROM mail_recipients r "
            "JOIN mails m ON m.id = r.mail_id "
            "WHERE r.sender='" + sender + "' "
            "AND r.recipient='" + recipient + "' "
            "AND m.subject='" + subject + "' "
            "AND TIMESTAMPDIFF(SECOND, m.send_time, NOW()) BETWEEN 0 AND " + std::to_string(kInboundDedupWindowSeconds) + " "
            "LIMIT 1";
        auto result = conn->query(sql);
        if (!(result && result->get_row_count() > 0)) {
            return false;
        }
    }

    return true;
}
#endif

bool PersistentQueue::batch_insert_attachments(mail* mail_data, std::string& error) {
    if (!mail_data || mail_data->attachments.empty()) {
        return true; // 没有附件不算错误
    }

    auto mysql_pool = std::dynamic_pointer_cast<MySQLPool>(db_pool_);
    auto connection = mysql_pool->get_raii_connection();
    auto conn = std::dynamic_pointer_cast<MySQLConnection>(connection.get());
    if (!conn || !conn->is_connected()) {
        error = "Failed to get database connection for attachments";
        LOG_PERSISTENT_QUEUE_ERROR("Failed to get database connection for attachments of mail ID {}", mail_data->id);
        return false;
    }

    try {
        std::string sql = "INSERT INTO attachments (mail_id, filename, filepath, file_size, mime_type) VALUES";
        for (size_t i = 0; i < mail_data->attachments.size(); ++i) {
            const auto& att = mail_data->attachments[i];
            sql += " (" + std::to_string(mail_data->id) + ", '" +
                conn->escape_string(att.filename) + "', '" +
                conn->escape_string(att.filepath) + "', " +
                std::to_string(att.file_size) + ", '" +
                conn->escape_string(att.mime_type) + "')";
            
            if (i < mail_data->attachments.size() - 1) {
                sql += ", ";
            }
        }
        
        if (!conn->execute(sql)) {
            error = "Failed to insert mail's attachment metadata";
            LOG_PERSISTENT_QUEUE_ERROR("Failed to insert attachment metadata for mail ID {} with sql: {}", 
                                      mail_data->id, sql);
            return false;
        }

        LOG_PERSISTENT_QUEUE_INFO("Successfully inserted {} attachments for mail ID {}", 
                                 mail_data->attachments.size(), mail_data->id);
        return true;
    } catch (const std::exception& e) {
        error = std::string("Exception during attachment insertion: ") + e.what();
        LOG_PERSISTENT_QUEUE_ERROR("Exception during attachment insertion for mail ID {}: {}", 
                                  mail_data->id, e.what());
        return false;
    }
    // RAIIConnection 析构时自动释放连接
}

bool PersistentQueue::batch_delete_metadata(mail* mail_data, std::string& error) {
    if (!mail_data) {
        error = "Mail data is null";
        return false;
    }

    auto mysql_pool = std::dynamic_pointer_cast<MySQLPool>(db_pool_);
    auto connection = mysql_pool->get_raii_connection();
    auto conn = std::dynamic_pointer_cast<MySQLConnection>(connection.get());
    if (!conn || !conn->is_connected()) {
        error = "Failed to get database connection for deleting metadata";
        LOG_PERSISTENT_QUEUE_ERROR("Failed to get database connection for deleting metadata of mail ID {}", mail_data->id);
        return false;
    }

    try {
        // 第一步：删除邮件收发件人关系记录
        std::string recipient_sql = "DELETE FROM mail_recipients WHERE mail_id = " + std::to_string(mail_data->id);
        if (!conn->execute(recipient_sql)) {
            LOG_PERSISTENT_QUEUE_WARN("Failed to delete mail recipients for mail ID {}", mail_data->id);
        }

        // 第二步：删除邮件元数据（会级联删除附件）
        std::string mail_sql = "DELETE FROM mails WHERE id = " + std::to_string(mail_data->id);
        if (!conn->execute(mail_sql)) {
            error = "Failed to delete mail metadata";
            LOG_PERSISTENT_QUEUE_ERROR("Failed to delete mail metadata for mail ID {}", mail_data->id);
            return false;
        }

        LOG_PERSISTENT_QUEUE_INFO("Successfully deleted metadata for mail ID {}", mail_data->id);
        return true;
    } catch (const std::exception& e) {
        error = std::string("Exception during metadata deletion: ") + e.what();
        LOG_PERSISTENT_QUEUE_ERROR("Exception during metadata deletion for mail ID {}: {}", 
                                  mail_data->id, e.what());
        return false;
    }
    // RAIIConnection 析构时自动释放连接
}

bool PersistentQueue::batch_delete_attachments(mail* mail_data, std::string& error) {
    if (!mail_data || mail_data->attachments.empty()) {
        return true; // 没有附件不算错误
    }

    auto mysql_pool = std::dynamic_pointer_cast<MySQLPool>(db_pool_);
    auto connection = mysql_pool->get_raii_connection();
    auto conn = std::dynamic_pointer_cast<MySQLConnection>(connection.get());
    if (!conn || !conn->is_connected()) {
        error = "Failed to get database connection for deleting attachments";
        LOG_PERSISTENT_QUEUE_ERROR("Failed to get database connection for deleting attachments of mail ID {}", mail_data->id);
        return false;
    }

    std::string sql = "DELETE FROM attachments WHERE mail_id = " + std::to_string(mail_data->id);
    if (!conn->execute(sql)) {
        error = "Failed to delete attachment metadata";
        LOG_PERSISTENT_QUEUE_ERROR("Failed to delete attachment metadata for mail ID {}", mail_data->id);
        return false;
    }

    LOG_PERSISTENT_QUEUE_INFO("Successfully deleted attachments for mail ID {}", mail_data->id);
    return true;
    // RAIIConnection 析构时自动释放连接
}

void PersistentQueue::worker_loop() {
    LOG_PERSISTENT_QUEUE_INFO("PersistentQueue worker thread started");
    while (process_task()) {
        // process_task() blocks on condition_variable, so no busy spin in idle state.
    }
    // 不在这里打印退出日志，因为可能在析构阶段调用，此时 Logger 可能已 shutdown
}

void PersistentQueue::enqueue_outbox_tasks(mail* mail_data) {
    if (!mail_data || !outbound_client_) {
        return;
    }

    outbound::OutboxRepository repository(db_pool_);
    if (!repository.enqueue_from_mail(*mail_data, local_domain_, nullptr)) {
        return;
    }
    outbound_client_->notify_outbox_ready();
}

// bool PersistentQueue::save_mail_body(mail* mail_data, const std::string& file_path, std::string& error) {
//     if (!mail_data) {
//         error = "Mail data is null";
//         return false;
//     }

//     if (file_path.empty()) {
//         error = "File path is empty";
//         return false;
//     }

//     try {
//         std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
//         if (!out.is_open()) {
//             error = "Failed to open file for writing: " + file_path;
//             LOG_PERSISTENT_QUEUE_ERROR("Failed to open file for writing: {}", file_path);
//             return false;
//         }

//         // 写入邮件正文
//         if (!mail_data->body.empty()) {
//             out.write(mail_data->body.data(), static_cast<std::streamsize>(mail_data->body.size()));
//             if (!out.good()) {
//                 error = "Failed to write mail body to file: " + file_path;
//                 LOG_PERSISTENT_QUEUE_ERROR("Failed to write mail body to file: {}", file_path);
//                 out.close();
//                 return false;
//             }
//         }

//         out.close();
//         LOG_PERSISTENT_QUEUE_INFO("Successfully saved mail body to file: {}", file_path);
//         return true;
//     } catch (const std::exception& e) {
//         error = std::string("Exception while saving mail body: ") + e.what();
//         LOG_PERSISTENT_QUEUE_ERROR("Exception while saving mail body for mail ID {}: {}", 
//                                   mail_data->id, e.what());
//         return false;
//     }
// }

// bool PersistentQueue::save_attachments(mail* mail_data, std::string& error) {
//     if (!mail_data || mail_data->attachments.empty()) {
//         return true; // 没有附件不算错误
//     }

//     try {
//         for (auto& att : mail_data->attachments) {
//             // 如果附件已经保存到文件（filepath已设置），跳过
//             if (!att.filepath.empty() && att.content.empty()) {
//                 LOG_PERSISTENT_QUEUE_DEBUG("Attachment {} already saved to file: {}", 
//                                           att.filename, att.filepath);
//                 continue;
//             }

//             // 如果filepath为空但有内容，需要保存
//             if (att.filepath.empty() && !att.content.empty()) {
//                 error = "Attachment filepath is empty but content exists: " + att.filename;
//                 LOG_PERSISTENT_QUEUE_ERROR("Attachment filepath is empty but content exists: {}", att.filename);
//                 return false;
//             }

//             // 如果有filepath且有content，保存content到文件
//             if (!att.filepath.empty() && !att.content.empty()) {
//                 std::ofstream out(att.filepath, std::ios::binary | std::ios::trunc);
//                 if (!out.is_open()) {
//                     error = "Failed to open attachment file for writing: " + att.filepath;
//                     LOG_PERSISTENT_QUEUE_ERROR("Failed to open attachment file for writing: {}", att.filepath);
//                     return false;
//                 }

//                 out.write(att.content.data(), static_cast<std::streamsize>(att.content.size()));
//                 if (!out.good()) {
//                     error = "Failed to write attachment to file: " + att.filepath;
//                     LOG_PERSISTENT_QUEUE_ERROR("Failed to write attachment to file: {}", att.filepath);
//                     out.close();
//                     return false;
//                 }

//                 out.close();
//                 att.file_size = att.content.size();
//                 att.content.clear(); // 释放内存
//                 LOG_PERSISTENT_QUEUE_INFO("Successfully saved attachment {} to file: {}", 
//                                          att.filename, att.filepath);
//             }
//         }

//         LOG_PERSISTENT_QUEUE_INFO("Successfully processed all attachments for mail ID {}", mail_data->id);
//         return true;
//     } catch (const std::exception& e) {
//         error = std::string("Exception while saving attachments: ") + e.what();
//         LOG_PERSISTENT_QUEUE_ERROR("Exception while saving attachments for mail ID {}: {}", 
//                                   mail_data->id, e.what());
//         return false;
//     }
// }

} // namespace persist_storage
} // namespace mail_system
