#include "mail_system/back/persist_storage/persistent_queue.h"
#include "mail_system/back/algorithm/snow.h"
#include "mail_system/back/algorithm/smtp_utils.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <cstring>

namespace mail_system {
namespace persist_storage {

PersistentQueue::PersistentQueue(
    std::shared_ptr<DBPool> db_pool,
    std::shared_ptr<ThreadPoolBase> worker_pool
) : db_pool_(db_pool), worker_pool_(worker_pool), shutdown_(false), worker_running_(false), current_task_count_(0), task_queue_(MAX_TASK_COUNT) {
    // 启动工作线程
    worker_running_ = true;
    worker_thread_ = std::thread(&PersistentQueue::worker_loop, this);
    LOG_PERSISTENT_QUEUE_INFO("PersistentQueue initialized and worker thread started");
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

    if (current_task_count_.load() >= MAX_TASK_COUNT) {
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
        current_task_count_.fetch_add(1);
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
    
    if (current_task_count_.load() + mail_list.size() > MAX_TASK_COUNT) {
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
        current_task_count_.fetch_add(mail_list.size());
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
    return current_task_count_.load();
}

void PersistentQueue::shutdown() {
    if (shutdown_.load()) {
        return;
    }
    shutdown_ = true;
    worker_running_ = false;
    // 唤醒可能在等待中的工作线程以便正常退出
    queue_cv_.notify_all();
    // 不在这里打印日志，因为可能在析构阶段调用，此时 Logger 可能已 shutdown
}

void PersistentQueue::process_task() {
    if (is_shutdown() || current_task_count_.load() == 0) {
        return;
    }
    mail* mail_data = nullptr;
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_cv_.wait(lock, [this] { return !task_queue_.empty() || is_shutdown(); });
        mail_data = task_queue_.front();
        task_queue_.erase(task_queue_.begin());
        current_task_count_.fetch_sub(1);

        if (mail_data->persist_status != PersistStatus::PENDING) {
            LOG_PERSISTENT_QUEUE_WARN("Mail ID {} has already been processed with status {}, skipping", mail_data->id, static_cast<int>(mail_data->persist_status));
            return;
        }
    }

    worker_pool_->submit([this, mail_data]() {
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
            LOG_PERSISTENT_QUEUE_INFO("Successfully processed mail ID {}", mail_data->id);
        } else {
            mail_data->persist_status = PersistStatus::FAILED;
            LOG_PERSISTENT_QUEUE_ERROR("Processing failed for mail ID {}", mail_data->id);
        }
    });
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
                            "', " + (mail_data->to[i] == "@localhost" ? "1" : "2") + "),";
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
    while (worker_running_) {
        process_task();
        // if (current_task_count_.load() * 1.5 < MAX_TASK_COUNT) {
        //     std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // } else {
        //     std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // }
    }
    // 不在这里打印退出日志，因为可能在析构阶段调用，此时 Logger 可能已 shutdown
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
