#ifndef SMTPS_FSM_H
#define SMTPS_FSM_H

#include "mail_system/back/mailServer/session/session_base.h"
#include "mail_system/back/db/db_pool.h"
#include "mail_system/back/db/db_service.h"
#include "mail_system/back/db/mysql_service.h"
#include "mail_system/back/thread_pool/thread_pool_base.h"
#include "mail_system/back/entities/mail.h"
#include "mail_system/back/common/logger.h"
#include <cstddef>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include <fstream>

namespace mail_system {

// SMTPS 状态枚举
enum class SmtpsState {
    INIT = 0,
    GREETING = 1,
    WAIT_EHLO = 2,
    WAIT_AUTH = 3,
    WAIT_AUTH_USERNAME = 4,
    WAIT_AUTH_PASSWORD = 5,
    WAIT_MAIL_FROM = 6,
    WAIT_RCPT_TO = 7,
    WAIT_DATA = 8,
    IN_MESSAGE = 9,
    WAIT_QUIT = 10,
    CLOSED = 11
};

// SMTPS 事件枚举
enum class SmtpsEvent {
    CONNECT = 0,
    EHLO = 1,
    AUTH = 2,
    MAIL_FROM = 3,
    RCPT_TO = 4,
    DATA = 5,
    DATA_END = 6,
    QUIT = 7,
    STARTTLS = 8,
    ERROR = 9,
    TIMEOUT = 10
};

// SMTPS 上下文
struct SmtpsContext {
    bool is_authenticated = false;
    std::string client_username;
    std::string sender_address;
    std::vector<std::string> recipient_addresses;
    std::string mail_data;
    // streaming / MIME parsing helpers
    bool header_parsed = false;
    bool streaming_enabled = false;
    bool multipart = false;
    bool has_attachment = true;
    std::string boundary;
    std::string header_buffer;
    std::string line_buffer;
    std::string text_body_buffer;
    size_t text_body_size = 0;
    size_t buffered_body_size = 0;
    bool body_limit_exceeded = false;
    std::string abort_reason;
    std::string current_part_headers;
    bool in_part_header = false;
    bool current_part_is_attachment = false;
    std::string current_part_encoding;
    std::string current_part_mime;
    std::string current_attachment_filename;
    std::string current_attachment_path;
    std::ofstream current_attachment_stream;
    std::string base64_remainder;
    size_t current_attachment_size = 0;
    std::vector<attachment> streamed_attachments;

    void clear() {
        is_authenticated = false;
        client_username.clear();
        sender_address.clear();
        recipient_addresses.clear();
        mail_data.clear();
        header_parsed = false;
        streaming_enabled = false;
        multipart = false;
        has_attachment = false;
        text_body_size = 0;
        buffered_body_size = 0;
        body_limit_exceeded = false;
        abort_reason.clear();
        boundary.clear();
        header_buffer.clear();
        line_buffer.clear();
        text_body_buffer.clear();
        current_part_headers.clear();
        in_part_header = false;
        current_part_is_attachment = false;
        current_part_encoding.clear();
        current_part_mime.clear();
        current_attachment_filename.clear();
        current_attachment_path.clear();
        base64_remainder.clear();
        current_attachment_size = 0;
        streamed_attachments.clear();
        if (current_attachment_stream.is_open()) {
            current_attachment_stream.close();
        }
    }
};

// 状态处理函数类型定义
template <typename ConnectionType>
using StateHandler = std::function<void(std::unique_ptr<SessionBase<ConnectionType>>, const std::string&)>;
// 会话处理器类型定义（用于unique_ptr）
template <typename ConnectionType>
using SessionHandler = std::function<void(std::unique_ptr<SessionBase<ConnectionType>>, const std::string&)>;

// SMTPS状态机接口
template <typename ConnectionType>
class SmtpsFsm {
protected:
    std::shared_ptr<ThreadPoolBase> m_ioThreadPool;
    std::shared_ptr<ThreadPoolBase> m_workerThreadPool;
    std::shared_ptr<DBPool> m_dbPool;
public:
    SmtpsFsm(std::shared_ptr<ThreadPoolBase> io_thread_pool,
             std::shared_ptr<ThreadPoolBase> worker_thread_pool,
             std::shared_ptr<DBPool> db_pool)
        : m_ioThreadPool(io_thread_pool),
          m_workerThreadPool(worker_thread_pool),
          m_dbPool(db_pool) {}
    virtual ~SmtpsFsm() = default;

    // 处理事件
    virtual void process_event(std::unique_ptr<SessionBase<ConnectionType>> session, SmtpsEvent event, const std::string& args) = 0;

    // 获取状态名称
    static std::string get_state_name(SmtpsState state) {
        static const std::unordered_map<SmtpsState, std::string> state_names = {
            {SmtpsState::INIT, "INIT"},
            {SmtpsState::GREETING, "GREETING"},
            {SmtpsState::WAIT_EHLO, "WAIT_EHLO"},
            {SmtpsState::WAIT_AUTH, "WAIT_AUTH"},
            {SmtpsState::WAIT_AUTH_USERNAME, "WAIT_AUTH_USERNAME"},
            {SmtpsState::WAIT_AUTH_PASSWORD, "WAIT_AUTH_PASSWORD"},
            {SmtpsState::WAIT_MAIL_FROM, "WAIT_MAIL_FROM"},
            {SmtpsState::WAIT_RCPT_TO, "WAIT_RCPT_TO"},
            {SmtpsState::WAIT_DATA, "WAIT_DATA"},
            {SmtpsState::IN_MESSAGE, "IN_MESSAGE"},
            {SmtpsState::WAIT_QUIT, "WAIT_QUIT"},
            {SmtpsState::CLOSED, "CLOSED"}
        };
        auto it = state_names.find(state);
        if (it != state_names.end()) {
            return it->second;
        }
        return "UNKNOWN_STATE";
    }

    // 获取事件名称
    static std::string get_event_name(SmtpsEvent event) {
        static const std::unordered_map<SmtpsEvent, std::string> event_names = {
            {SmtpsEvent::CONNECT, "CONNECT"},
            {SmtpsEvent::EHLO, "EHLO"},
            {SmtpsEvent::AUTH, "AUTH"},
            {SmtpsEvent::STARTTLS, "STARTTLS"},
            {SmtpsEvent::MAIL_FROM, "MAIL_FROM"},
            {SmtpsEvent::RCPT_TO, "RCPT_TO"},
            {SmtpsEvent::DATA, "DATA"},
            {SmtpsEvent::DATA_END, "DATA_END"},
            {SmtpsEvent::QUIT, "QUIT"},
            {SmtpsEvent::ERROR, "ERROR"},
            {SmtpsEvent::TIMEOUT, "TIMEOUT"}
        };
        auto it = event_names.find(event);
        if (it != event_names.end()) {
            return it->second;
        }
        return "UNKNOWN_EVENT";
    }

    // 数据库操作

    bool auth_user(SessionBase<ConnectionType>* session, const std::string& mail_address, const std::string& password) {
        if (!session) {
            LOG_AUTH_ERROR("Session is null in auth_user");
            return false;
        }
        auto connection = m_dbPool->get_connection();
        if (!connection || !connection->is_connected()) {
            LOG_AUTH_ERROR("Failed to get database connection in auth_user");
            return false;
        }

        // 使用参数化查询防止SQL注入并提高性能
        std::string sql = "SELECT COUNT(*) as cnt FROM users WHERE mail_address = ? AND password = ?";
        auto result = connection->query(sql, {mail_address, password});
        if (!result || result->get_row_count() == 0) {
            return false;
        }
        return std::stoul(result->get_value(0, "cnt")) > 0;
    }

    bool auth_user(std::unique_ptr<SessionBase<ConnectionType>> session, const std::string& mail_address, const std::string& password) {
        return auth_user(session.get(), mail_address, password);
    }

    void get_mail_data(SessionBase<ConnectionType>* session, std::string& mail_data) {
        if (!session) {
            LOG_AUTH_ERROR("Session is null in get_mail_data");
            return;
        }

        auto connection = m_dbPool->get_connection();
        if (!connection || !connection->is_connected()) {
            LOG_AUTH_ERROR("Failed to get database connection in get_mail_data");
            return;
        }

        // 从session上下文获取发件人地址（如果已设置）
        std::string sender = session->context_.sender_address.empty() ? session->context_.client_username : session->context_.sender_address;

        // 使用参数化查询
        std::string sql = "SELECT subject, body FROM mails WHERE sender = ? ORDER BY send_time DESC LIMIT 1";
        auto result = connection->query(sql, {sender});
        if (result && result->get_row_count() > 0) {
            std::string subject = result->get_value(0, "subject");
            std::string body = result->get_value(0, "body");
            mail_data = "Subject: " + subject + "\n\n" + body;
        }
    }

    void get_mail_data(std::unique_ptr<SessionBase<ConnectionType>> session, std::string& mail_data) {
        get_mail_data(session.get(), mail_data);
    }

    // 保存邮件元数据到数据库（异步），返回future用于跟踪操作结果
    std::future<bool> save_mail_metadata_async(mail* data, const std::string& file_path_prefix) {
        std::future<bool> future;

        if (!data) {
            LOG_DATABASE_ERROR("Mail data is null in save_mail_metadata_async");
            return std::future<bool>();
        }

        // 复制 mail 数据，避免悬空指针问题
        mail mail_data = *data;

        auto task = [this, file_path_prefix, mail_data]() -> bool {
            auto connection = m_dbPool->get_connection();
            if (!connection || !connection->is_connected()) {
                LOG_DATABASE_ERROR("Failed to get database connection in async task");
                return false;
            }

            auto mysql_connection = std::dynamic_pointer_cast<MySQLConnection>(connection);

            bool success = true;

            for (const auto& recipient : mail_data.to) {
                std::string body_path = file_path_prefix;

                // 插入邮件元数据，使用自生成的ID
                std::string mail_sql = "INSERT INTO mails (id, sender, recipient, subject, body_path, status) VALUES (" +
                    std::to_string(mail_data.id) + ", '" +
                    mysql_connection->escape_string(mail_data.from) + "', '" +
                    mysql_connection->escape_string(recipient) + "', '" +
                    mysql_connection->escape_string(mail_data.subject) + "', '" +
                    mysql_connection->escape_string(body_path) + "', " +
                    std::to_string(mail_data.status) + + ")";
                LOG_DATABASE_INFO("Executing SQL: {}", mail_sql);
                if (!mysql_connection->execute(mail_sql)) {
                    LOG_DATABASE_ERROR("Failed to insert mail metadata. Error: {}", mysql_connection->get_last_error());
                    success = false;
                    break;
                }

                // 插入附件元数据
                for (const auto& att : mail_data.attachments) {
                    std::string att_sql = "INSERT INTO attachments (mail_id, filename, filepath, file_size, mime_type) VALUES (" +
                        std::to_string(mail_data.id) + ", '" +
                        mysql_connection->escape_string(att.filename) + "', '" +
                        mysql_connection->escape_string(att.filepath) + "', " +
                        std::to_string(att.file_size) + ", '" +
                        mysql_connection->escape_string(att.mime_type) + "')";
                    LOG_DATABASE_INFO("Executing SQL: {}", att_sql);
                    if (!mysql_connection->execute(att_sql)) {
                        LOG_DATABASE_ERROR("Failed to insert attachment metadata. Error: {}", mysql_connection->get_last_error());
                        success = false;
                        break;
                    }
                }

                if (!success) {
                    break;
                }
            }

            return success;
        };

        future = m_workerThreadPool->submit(std::move(task));

        return future;
    }

    // 根据文件路径删除邮件元数据 假定数据库操作一定成功
    void remove_metadata_by_file_path(const std::vector<std::string>& file_paths) {
        auto connection = m_dbPool->get_connection();
        if (!connection || !connection->is_connected()) {
            LOG_DATABASE_ERROR("Failed to get database connection in remove_metadata_by_file_path");
            return;
        }

        // 注意：数据库中 body_path 字段存储的是文件路径
        std::string sql = "DELETE FROM mails WHERE body_path = ?";
        for (const auto& file_path : file_paths) {
            if (!connection->execute(sql, {file_path})) {
                LOG_DATABASE_ERROR("Failed to delete mail metadata for file path: {}", file_path);
            }
        }
    }

    // 保存邮件正文到本地文件
    bool save_mail_body_to_file(mail* data, const std::string& file_path) {
        if (!data) {
            LOG_FILE_IO_ERROR("Mail data is null in save_mail_body_to_file");
            return false;
        }

        LOG_FILE_IO_INFO("Saving mail body to file: {}, body size: {}", file_path, data->body.size());

        std::ofstream out(file_path);
        if (!out.is_open()) {
            LOG_FILE_IO_ERROR("Failed to open file for writing: {}", file_path);
            return false;
        }

        if (data->body.empty()) {
            out << data->header;
            out.close();
            LOG_FILE_IO_INFO("Mail body is empty, only header saved to file: {}", file_path);
            return true;
        }
        out << data->body;
        out.close();
        LOG_FILE_IO_INFO("Mail body saved successfully to file: {}", file_path);
        return true;
    }

    bool save_attachment_to_file(attachment& att, const std::string& file_path) {
        std::ofstream out(file_path, std::ios::binary);
        if (!out.is_open()) {
            LOG_FILE_IO_ERROR("Failed to open attachment file for writing: {}", file_path);
            return false;
        }
        out.write(att.content.data(), static_cast<std::streamsize>(att.content.size()));
        out.close();
        att.filepath = file_path;
        att.file_size = att.content.size();
        att.content.clear(); // 释放内存
        LOG_FILE_IO_INFO("Attachment saved to file: {}", file_path);
        return true;
    }

    // 检查异步操作结果，失败则删除对应文件
    void cleanup_failed_saves(std::vector<std::future<bool>>& futures, const std::vector<std::string>& file_paths) {
        for (size_t i = 0; i < futures.size(); ++i) {
            if (futures[i].valid()) {
                bool success = futures[i].get();
                if (!success && i < file_paths.size()) {
                    LOG_FILE_IO_ERROR("Database save failed, removing file: {}", file_paths[i]);
                    std::remove(file_paths[i].c_str());
                }
            }
        }
    }
};

} // namespace mail_system

#endif // SMTPS_FSM_H