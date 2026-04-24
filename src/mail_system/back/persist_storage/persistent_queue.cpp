#include "mail_system/back/persist_storage/persistent_queue.h"
#include "mail_system/back/algorithm/snow.h"
#include "mail_system/back/algorithm/smtp_utils.h"
#include "mail_system/back/outbound/mx_routing_utils.h"
#include "mail_system/back/outbound/outbox_repository.h"
#include "mail_system/back/outbound/smtp_outbound_client.h"
#include <array>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <cstring>
#include <optional>
#include <unordered_map>
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#ifdef __linux__
#include <sys/sysinfo.h>
#endif

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

class ScopedMySQLConnection {
public:
    explicit ScopedMySQLConnection(const std::shared_ptr<DBPool>& pool)
        : pool_(pool) {
        if (pool_) {
            raw_connection_ = pool_->get_connection();
            mysql_connection_ = std::dynamic_pointer_cast<MySQLConnection>(raw_connection_);
        }
    }

    ~ScopedMySQLConnection() {
        if (pool_ && raw_connection_) {
            pool_->release_connection(raw_connection_);
        }
    }

    MySQLConnection* get() const {
        return mysql_connection_.get();
    }

    bool is_valid() const {
        return mysql_connection_ && mysql_connection_->is_connected();
    }

private:
    std::shared_ptr<DBPool> pool_;
    std::shared_ptr<IDBConnection> raw_connection_;
    std::shared_ptr<MySQLConnection> mysql_connection_;
};

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

std::optional<std::uint64_t> query_available_memory_bytes() {
#ifdef __APPLE__
    mach_port_t host_port = mach_host_self();
    vm_statistics64_data_t vm_stats{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(host_port, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm_stats), &count) != KERN_SUCCESS) {
        return std::nullopt;
    }

    const auto page_size = static_cast<std::uint64_t>(vm_kernel_page_size);
    return static_cast<std::uint64_t>(vm_stats.free_count + vm_stats.inactive_count) * page_size;
#elif defined(__linux__)
    struct sysinfo info {};
    if (sysinfo(&info) != 0) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(info.freeram) * static_cast<std::uint64_t>(info.mem_unit);
#else
    return std::nullopt;
#endif
}

} // namespace

PersistentQueue::PersistentQueue(
    std::shared_ptr<DBPool> db_pool,
    std::shared_ptr<ThreadPoolBase> worker_pool,
    std::shared_ptr<mail_system::storage::IStorageProvider> storage_provider
) : db_pool_(std::move(db_pool)),
    worker_pool_(std::move(worker_pool)),
    storage_provider_(std::move(storage_provider)),
    shutdown_(false) {
    // 启动工作线程
    worker_thread_ = std::thread(&PersistentQueue::worker_loop, this);
    LOG_PERSISTENT_QUEUE_INFO("PersistentQueue initialized and worker thread started");
}

PersistentQueue::~PersistentQueue() {
    shutdown();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

SubmitOwnedMailResult PersistentQueue::submit_owned_mail(std::unique_ptr<mail> mail_data) {
    SubmitOwnedMailResult result;
    result.rejected_mail = std::move(mail_data);

    if (!result.rejected_mail) {
        result.error = "mail is null";
        LOG_PERSISTENT_QUEUE_ERROR("Cannot submit mail, owned mail is null");
        return result;
    }

    if (is_shutdown()) {
        result.error = "persistent queue is shutdown";
        LOG_PERSISTENT_QUEUE_ERROR("Cannot submit mail, PersistentQueue is shutdown");
        return result;
    }

    if (std::string reason; should_reject_submission(*result.rejected_mail, reason)) {
        result.error = std::move(reason);
        LOG_PERSISTENT_QUEUE_WARN("PersistentQueue rejected mail_id={}, reason={}",
                                  result.rejected_mail->id,
                                  result.error);
        return result;
    }

    result.ticket.mail_id = result.rejected_mail->id;
    result.ticket.status = result.rejected_mail->persist_status;
    result.ticket.cancel_requested = std::make_shared<std::atomic<bool>>(false);

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        task_queue_.push_back(std::make_pair(std::move(result.rejected_mail), result.ticket));
        queued_task_count_.fetch_add(1, std::memory_order_relaxed);
        inflight_mail_count_.fetch_add(1, std::memory_order_relaxed);
    }

    queue_cv_.notify_one();
    result.accepted = true;
    LOG_PERSISTENT_QUEUE_INFO("Mail submitted to PersistentQueue, mail_id={}, queued={}, inflight={}",
                              result.ticket.mail_id,
                              queue_size(),
                              inflight_mail_count_.load(std::memory_order_relaxed));
    return result;
}

bool PersistentQueue::submit_mails(std::vector<mail*>& mail_list) {
    LOG_PERSISTENT_QUEUE_WARN("submit_mails(raw pointers) is deprecated and disabled for ownership safety");
    return false;
}

void PersistentQueue::delete_task(mail* mail_data) {
    if (!mail_data) {
        LOG_PERSISTENT_QUEUE_ERROR("Cannot delete task: mail_data is null");
        return;
    }

    ScopedMySQLConnection connection(db_pool_);
    auto* conn = connection.get();
    if (!connection.is_valid()) {
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
    // ScopedMySQLConnection 析构时自动释放连接
}

void PersistentQueue::delete_multi_tasks(std::vector<mail*>& mail_list) {
    if (mail_list.empty()) {
        return;
    }

    ScopedMySQLConnection connection(db_pool_);
    auto* conn = connection.get();
    if (!connection.is_valid()) {
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
    // ScopedMySQLConnection 析构时自动释放连接
}

size_t PersistentQueue::queue_size() const {
    return queued_task_count_.load(std::memory_order_relaxed);
}

void PersistentQueue::set_outbound_client(std::shared_ptr<mail_system::outbound::SmtpOutboundClient> outbound_client) {
    outbound_client_ = std::move(outbound_client);
}

void PersistentQueue::set_local_domain(std::string local_domain) {
    if (!local_domain.empty()) {
        local_domain_ = std::move(local_domain);
    }
}

void PersistentQueue::set_storage_provider(std::shared_ptr<mail_system::storage::IStorageProvider> storage_provider) {
    storage_provider_ = std::move(storage_provider);
}

void PersistentQueue::set_pressure_config(PersistentQueuePressureConfig config) {
    pressure_config_ = config;
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
    std::unique_ptr<mail> mail_data;
    PersistSubmissionTicket ticket;
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_cv_.wait(lock, [this] {
            return shutdown_.load(std::memory_order_relaxed) || !task_queue_.empty();
        });
        if (shutdown_.load(std::memory_order_relaxed) && task_queue_.empty()) {
            return false;
        }

        auto task = std::move(task_queue_.front());
        task_queue_.erase(task_queue_.begin());
        queued_task_count_.fetch_sub(1, std::memory_order_relaxed);
        mail_data = std::move(task.first);
        ticket = std::move(task.second);

        if (!mail_data) {
            inflight_mail_count_.fetch_sub(1, std::memory_order_relaxed);
            LOG_PERSISTENT_QUEUE_WARN("Skipped empty mail task");
            return true;
        }

        if (mail_data->persist_status != PersistStatus::PENDING) {
            LOG_PERSISTENT_QUEUE_WARN("Mail ID {} has already been processed with status {}, skipping", mail_data->id, static_cast<int>(mail_data->persist_status));
            inflight_mail_count_.fetch_sub(1, std::memory_order_relaxed);
            return true;
        }
    }

    worker_pool_->submit([this, ticket, mail_data = std::move(mail_data)]() mutable {
        std::string error;
        const auto finish = [this]() {
            inflight_mail_count_.fetch_sub(1, std::memory_order_relaxed);
        };

        if (!mail_data) {
            finish();
            return;
        }

        if (ticket.is_cancel_requested()) {
            mail_data->persist_status = PersistStatus::CANCELLED;
            cleanup_mail_files(mail_data.get());
            finish();
            return;
        }

        mail_data->persist_status = PersistStatus::PROCESSING;

        if (ticket.is_cancel_requested()) {
            mail_data->persist_status = PersistStatus::CANCELLED;
            cleanup_mail_files(mail_data.get());
            finish();
            return;
        }

        std::vector<outbound::OutboxRecord> reserved_records;
        const bool can_hot_dispatch =
            outbound_client_ &&
            outbound::has_external_recipient(*mail_data, local_domain_) &&
            !ticket.is_cancel_requested();
        const std::string reserve_owner = can_hot_dispatch ? outbound_client_->worker_id() : std::string{};
        const int reserve_lease_seconds =
            can_hot_dispatch ? outbound_client_->local_reservation_lease_seconds() : 0;

        if (persist_mail_transactional(mail_data.get(),
                                       reserve_owner,
                                       reserve_lease_seconds,
                                       &reserved_records,
                                       error)) {
            const auto mail_id = mail_data->id;
            if (ticket.is_cancel_requested()) {
                mail_data->persist_status = PersistStatus::CANCELLED;
                cleanup_failed_mail(mail_data.get());
                finish();
                return;
            }

            mail_data->persist_status = PersistStatus::SUCCESS;
            if (!mail_data->deduplicated_inbound &&
                outbound::has_external_recipient(*mail_data, local_domain_)) {
                if (can_hot_dispatch) {
                    std::vector<std::uint64_t> reserved_ids;
                    reserved_ids.reserve(reserved_records.size());
                    for (const auto& record : reserved_records) {
                        reserved_ids.push_back(record.id);
                    }
                    if (!outbound_client_->accept_reserved_mail_ownership(std::move(mail_data),
                                                                          std::move(reserved_records))) {
                        LOG_PERSISTENT_QUEUE_WARN("Failed to hand reserved outbox records to local outbound client, mail_id={}",
                                                  mail_id);
                        outbound::OutboxRepository repository(db_pool_);
                        if (!repository.release_local_reservations(reserved_ids)) {
                            LOG_PERSISTENT_QUEUE_WARN("Failed to release local outbox reservations for mail_id={}",
                                                      mail_id);
                        }
                        outbound_client_->notify_outbox_ready();
                    }
                } else if (outbound_client_) {
                    outbound_client_->notify_outbox_ready();
                }
            } else {
                cleanup_mail_files(mail_data.get());
            }
            LOG_PERSISTENT_QUEUE_INFO("Successfully processed mail ID {}", mail_id);
        } else {
            mail_data->persist_status = PersistStatus::FAILED;
            cleanup_failed_mail(mail_data.get());
            LOG_PERSISTENT_QUEUE_ERROR("Processing failed for mail ID {}: {}", mail_data->id, error);
        }

        finish();
    });

    return true;
}

bool PersistentQueue::batch_insert_metadata(mail* mail_data, std::string& error) {
    if (!mail_data) {
        error = "Mail data is null";
        return false;
    }

    ScopedMySQLConnection connection(db_pool_);
    auto* conn = connection.get();
    if (!connection.is_valid()) {
        error = "Failed to get database connection";
        LOG_PERSISTENT_QUEUE_ERROR("Failed to get database connection for mail ID {}", mail_data->id);
        return false;
    }

    return batch_insert_metadata(mail_data, conn, error);
}

bool PersistentQueue::batch_insert_metadata(mail* mail_data, MySQLConnection* conn, std::string& error) {
    if (!mail_data) {
        error = "Mail data is null";
        return false;
    }
    if (!conn) {
        error = "Database connection is null";
        return false;
    }

    try {
        std::string mail_sql = "INSERT INTO mails (id, subject, body_path) VALUES (" +
                              std::to_string(mail_data->id) + ", '" +
                              conn->escape_string(mail_data->subject) + "', '" +
                              conn->escape_string(mail_data->body_path) + "'" + ")";

        if (!conn->execute(mail_sql)) {
            error = "Failed to insert mail metadata";
            LOG_PERSISTENT_QUEUE_ERROR("Failed to insert mail metadata with sql: {}", mail_sql);
            return false;
        }

        std::string recipient_sql = "INSERT INTO mail_recipients (id, mail_id, sender, recipient, status, source_message_id) VALUES";
        const std::string source_message_id = conn->escape_string(mail_data->source_message_id);
        for (size_t i = 0; i < mail_data->to.size(); ++i) {
            recipient_sql += " (" + std::to_string(mail_data->ids[i]) + ", " +
                            std::to_string(mail_data->id) + ", '" +
                            conn->escape_string(mail_data->from) + "', '" +
                            conn->escape_string(mail_data->to[i]) +
                            "', " + std::to_string(recipient_status_for_domain(mail_data->to[i], local_domain_)) +
                            ", '" + source_message_id + "'),";
        }
        recipient_sql[recipient_sql.size() - 1] = ';';

        if (!conn->execute(recipient_sql)) {
            error = "Failed to insert mail recipients";
            LOG_PERSISTENT_QUEUE_ERROR("Failed to insert mail recipients with sql: {}", recipient_sql);
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

bool PersistentQueue::is_duplicate_by_source_message_id(mail* mail_data, MySQLConnection* conn) {
    if (!mail_data || !conn || mail_data->source_message_id.empty() || mail_data->to.empty()) {
        return false;
    }

    const std::string sender = conn->escape_string(mail_data->from);
    const std::string message_id = conn->escape_string(mail_data->source_message_id);

    for (const auto& recipient_raw : mail_data->to) {
        const std::string recipient = conn->escape_string(recipient_raw);
        const std::string sql =
            "SELECT 1 FROM mail_recipients "
            "WHERE sender='" + sender + "' "
            "AND recipient='" + recipient + "' "
            "AND source_message_id='" + message_id + "' "
            "LIMIT 1";
        auto result = conn->query(sql);
        if (!(result && result->get_row_count() > 0)) {
            return false;
        }
    }

    return true;
}

bool PersistentQueue::batch_insert_attachments(mail* mail_data, std::string& error) {
    if (!mail_data || mail_data->attachments.empty()) {
        return true; // 没有附件不算错误
    }

    ScopedMySQLConnection connection(db_pool_);
    auto* conn = connection.get();
    if (!connection.is_valid()) {
        error = "Failed to get database connection for attachments";
        LOG_PERSISTENT_QUEUE_ERROR("Failed to get database connection for attachments of mail ID {}", mail_data->id);
        return false;
    }

    return batch_insert_attachments(mail_data, conn, error);
}

bool PersistentQueue::batch_insert_attachments(mail* mail_data, MySQLConnection* conn, std::string& error) {
    if (!mail_data || mail_data->attachments.empty()) {
        return true;
    }
    if (!conn) {
        error = "Database connection is null";
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
}

bool PersistentQueue::batch_delete_metadata(mail* mail_data, std::string& error) {
    if (!mail_data) {
        error = "Mail data is null";
        return false;
    }

    ScopedMySQLConnection connection(db_pool_);
    auto* conn = connection.get();
    if (!connection.is_valid()) {
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
    // ScopedMySQLConnection 析构时自动释放连接
}

bool PersistentQueue::batch_delete_attachments(mail* mail_data, std::string& error) {
    if (!mail_data || mail_data->attachments.empty()) {
        return true; // 没有附件不算错误
    }

    ScopedMySQLConnection connection(db_pool_);
    auto* conn = connection.get();
    if (!connection.is_valid()) {
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
    // ScopedMySQLConnection 析构时自动释放连接
}

void PersistentQueue::worker_loop() {
    LOG_PERSISTENT_QUEUE_INFO("PersistentQueue worker thread started");
    while (process_task()) {
        // process_task() blocks on condition_variable, so no busy spin in idle state.
    }
    // 不在这里打印退出日志，因为可能在析构阶段调用，此时 Logger 可能已 shutdown
}

bool PersistentQueue::enqueue_outbox_tasks(const mail& mail_data) {
    outbound::OutboxRepository repository(db_pool_);
    if (!repository.enqueue_from_mail(mail_data, local_domain_, nullptr)) {
        return !outbound::has_external_recipient(mail_data, local_domain_);
    }

    if (outbound_client_) {
        outbound_client_->notify_outbox_ready();
    }
    return true;
}

bool PersistentQueue::enqueue_outbox_tasks(mail* mail_data,
                                           MySQLConnection* conn,
                                           const std::string& reserve_owner,
                                           int reserve_lease_seconds,
                                           std::vector<outbound::OutboxRecord>* reserved_records,
                                           std::string& error) {
    if (!mail_data || !conn) {
        error = "Mail data or connection is null";
        return false;
    }

    bool inserted_any = false;
    const bool reserve_for_local = !reserve_owner.empty() && reserve_lease_seconds > 0;
    const auto escaped_sender = conn->escape_string(mail_data->from);

    for (const auto& recipient : mail_data->to) {
        const auto recipient_domain = extract_domain_lower(recipient);
        const auto local_domain = extract_domain_lower(local_domain_);
        if (recipient_domain.empty() || recipient_domain == local_domain) {
            continue;
        }

        std::ostringstream sql;
        if (reserve_for_local) {
            sql << "INSERT INTO mail_outbox (mail_id, sender, recipient, status, priority, attempt_count, max_attempts, next_attempt_at, lease_owner, lease_until) VALUES ("
                << mail_data->id << ", '" << escaped_sender << "', '"
                << conn->escape_string(recipient) << "', "
                << static_cast<int>(outbound::OutboxStatus::SENDING) << ", 0, 1, 8, NOW(), '"
                << conn->escape_string(reserve_owner) << "', DATE_ADD(NOW(), INTERVAL "
                << std::max(1, reserve_lease_seconds) << " SECOND))";
        } else {
            sql << "INSERT INTO mail_outbox (mail_id, sender, recipient, status, priority, attempt_count, max_attempts, next_attempt_at) VALUES ("
                << mail_data->id << ", '" << escaped_sender << "', '"
                << conn->escape_string(recipient) << "', "
                << static_cast<int>(outbound::OutboxStatus::PENDING) << ", 0, 0, 8, NOW())";
        }

        if (!conn->execute(sql.str())) {
            error = "Failed to insert outbox row";
            LOG_PERSISTENT_QUEUE_ERROR("Failed to insert outbox row for mail ID {} recipient {}: {}",
                                      mail_data->id,
                                      recipient,
                                      conn->get_last_error());
            return false;
        }

        inserted_any = true;
        if (reserved_records) {
            auto result = conn->query("SELECT LAST_INSERT_ID() AS id");
            if (!(result && result->get_row_count() > 0)) {
                error = "Failed to fetch LAST_INSERT_ID for outbox row";
                return false;
            }
            auto row = result->get_row(0);
            auto it = row.find("id");
            if (it == row.end()) {
                error = "LAST_INSERT_ID result missing id";
                return false;
            }

            outbound::OutboxRecord record;
            record.id = static_cast<std::uint64_t>(std::stoull(it->second));
            record.mail_id = mail_data->id;
            record.sender = mail_data->from;
            record.recipient = recipient;
            record.body_path = mail_data->body_path;
            record.attempt_count = reserve_for_local ? 1 : 0;
            record.max_attempts = 8;
            reserved_records->push_back(std::move(record));
        }
    }

    if (!inserted_any) {
        return true;
    }

    return true;
}

bool PersistentQueue::persist_mail_transactional(mail* mail_data,
                                                 const std::string& reserve_owner,
                                                 int reserve_lease_seconds,
                                                 std::vector<outbound::OutboxRecord>* reserved_records,
                                                 std::string& error) {
    if (!mail_data) {
        error = "Mail data is null";
        return false;
    }

    ScopedMySQLConnection scoped_conn(db_pool_);
    auto* conn = scoped_conn.get();
    if (!scoped_conn.is_valid()) {
        error = "Failed to get database connection";
        LOG_PERSISTENT_QUEUE_ERROR("Failed to get database connection for message-id dedup check, mail_id={}",
                                   mail_data->id);
        return false;
    }

    if (is_duplicate_by_source_message_id(mail_data, conn)) {
        mail_data->deduplicated_inbound = true;
        LOG_PERSISTENT_QUEUE_INFO("Message-ID dedup hit, skip all persistence for mail_id={}, message_id=[{}]",
                                  mail_data->id,
                                  mail_data->source_message_id);
        return true;
    }

    if (!conn->begin_transaction()) {
        error = "Failed to begin transaction";
        return false;
    }

    const auto rollback = [&]() {
        if (!conn->rollback()) {
            LOG_PERSISTENT_QUEUE_WARN("Failed to rollback transaction for mail_id={}", mail_data->id);
        }
    };

    if (!batch_insert_metadata(mail_data, conn, error)) {
        rollback();
        return false;
    }

    if (!batch_insert_attachments(mail_data, conn, error)) {
        rollback();
        return false;
    }

    if (!enqueue_outbox_tasks(mail_data, conn, reserve_owner, reserve_lease_seconds, reserved_records, error)) {
        rollback();
        return false;
    }

    if (!conn->commit()) {
        error = "Failed to commit transaction";
        rollback();
        return false;
    }

    return true;
}

void PersistentQueue::cleanup_mail_files(mail* mail_data) {
    if (!mail_data) {
        return;
    }

    if (!mail_data->body_path.empty()) {
        if (storage_provider_) {
            std::string error;
            if (!storage_provider_->remove_object(mail_data->body_path, error)) {
                LOG_PERSISTENT_QUEUE_WARN("Failed to delete mail body object: {}, error={}",
                                          mail_data->body_path,
                                          error);
            }
        } else if (std::remove(mail_data->body_path.c_str()) != 0) {
            LOG_PERSISTENT_QUEUE_WARN("Failed to delete mail body file: {}", mail_data->body_path);
        }
    }

    for (const auto& attachment : mail_data->attachments) {
        if (attachment.filepath.empty()) {
            continue;
        }
        if (storage_provider_) {
            std::string error;
            if (!storage_provider_->remove_object(attachment.filepath, error)) {
                LOG_PERSISTENT_QUEUE_WARN("Failed to delete attachment object: {}, error={}",
                                          attachment.filepath,
                                          error);
            }
        } else if (std::remove(attachment.filepath.c_str()) != 0) {
            LOG_PERSISTENT_QUEUE_WARN("Failed to delete attachment file: {}", attachment.filepath);
        }
    }
}

void PersistentQueue::cleanup_failed_mail(mail* mail_data) {
    if (!mail_data) {
        return;
    }

    std::string error;
    batch_delete_attachments(mail_data, error);
    error.clear();
    batch_delete_metadata(mail_data, error);
    cleanup_mail_files(mail_data);
}

bool PersistentQueue::should_reject_submission(const mail& mail_data, std::string& reason) {
    (void)mail_data;
    if (pressure_config_.max_inflight_mails > 0 &&
        inflight_mail_count_.load(std::memory_order_relaxed) >= pressure_config_.max_inflight_mails) {
        reason = "persist inflight limit reached";
        return true;
    }

    if (is_db_under_pressure(reason)) {
        return true;
    }

    if (is_memory_under_pressure(reason)) {
        return true;
    }

    return false;
}

bool PersistentQueue::is_db_under_pressure(std::string& reason) const {
    if (!db_pool_ || pressure_config_.min_db_available_connections == 0) {
        return false;
    }

    const auto available = db_pool_->get_available_connections();
    if (available >= pressure_config_.min_db_available_connections) {
        return false;
    }

    reason = "database available connections below threshold";
    return true;
}

bool PersistentQueue::is_memory_under_pressure(std::string& reason) const {
    if (pressure_config_.min_available_memory_mb == 0) {
        return false;
    }

    const auto available_bytes = query_available_memory_bytes();
    if (!available_bytes.has_value()) {
        return false;
    }

    const auto threshold_bytes = static_cast<std::uint64_t>(pressure_config_.min_available_memory_mb) * 1024ULL * 1024ULL;
    if (available_bytes.value() >= threshold_bytes) {
        return false;
    }

    reason = "available memory below threshold";
    return true;
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
