#pragma once

#include<iostream>
#include<string>
#include<vector>
#include<ctime>
#include <atomic>
#include <fstream>
#include <unordered_map>
#include <future>

namespace mail_system {
namespace persist_storage {
enum class PersistStatus : int {
    PENDING = 0,
    PROCESSING = 1,
    SUCCESS = 2,
    FAILED = 3,
    CANCELLED = 4
};

class SharedPersistStatus {
public:
    SharedPersistStatus()
        : value_(std::make_shared<std::atomic<int>>(static_cast<int>(PersistStatus::PENDING))) {}

    SharedPersistStatus(const SharedPersistStatus&) = default;
    SharedPersistStatus(SharedPersistStatus&&) noexcept = default;
    SharedPersistStatus& operator=(const SharedPersistStatus&) = default;
    SharedPersistStatus& operator=(SharedPersistStatus&&) noexcept = default;

    SharedPersistStatus& operator=(PersistStatus status) {
        store(status);
        return *this;
    }

    operator PersistStatus() const {
        return load();
    }

    explicit operator int() const {
        return static_cast<int>(load());
    }

    PersistStatus load() const {
        return static_cast<PersistStatus>(value_->load(std::memory_order_acquire));
    }

    void store(PersistStatus status) const {
        value_->store(static_cast<int>(status), std::memory_order_release);
    }

    std::shared_ptr<std::atomic<int>> share() const {
        return value_;
    }

private:
    std::shared_ptr<std::atomic<int>> value_;
};
}
}

struct mailbox
{
    size_t id;                 // 主键
    size_t user_id;            // 所属用户ID
    std::string name;          // 邮箱名称
    bool is_system;            // 是否为系统默认邮箱（收件箱、发件箱、垃圾箱等）
    int box_type;              // 系统邮箱类型：1收件箱，2发件箱，3垃圾箱，4草稿箱，5已删除
    time_t create_time;        // 创建时间
};

struct attachment
{
    size_t id;                  // 主键
    size_t mail_id;            // 所属邮件ID
    std::string filename;       // 文件名
    std::string filepath;       // 文件存储路径
    size_t file_size;          // 文件大小（字节）
    std::string mime_type;     // 文件MIME类型
    time_t upload_time;        // 上传时间
    std::string content;       // 解析阶段暂存的原始内容（落盘后可清空）
    std::shared_future<bool> meta_future; // 附件元数据持久化返回的 future，可拷贝
};

struct mail
{
    size_t id;                  // 主键
    std::vector<size_t> ids;    // 关系的主键
    std::string from;           // 发件人邮箱地址
    std::vector<std::string> to;// 收件人邮箱地址
    std::string header;         // 邮件头
    std::string body;           // 邮件正文
    std::string subject;        // 邮件主题
    std::string source_message_id; // 上游MTA的Message-ID，用于幂等去重
    time_t send_time;           // 发送时间
    int box_id;                 // 邮件所属邮箱ID
    int status;                 // 邮件状态：0已读，1未读，2未送达，3草稿，4垃圾邮件，5已删除
    std::string body_path;      // 邮件正文存储路径（使用文件存储）
    std::vector<attachment> attachments; // 附件元数据列表
    mail_system::persist_storage::SharedPersistStatus persist_status{}; // 持久化状态
    bool mail_over{false};    // 邮件内容是否完整（用于流式处理）
    bool deduplicated_inbound{false}; // 是否被入站去重命中
    std::shared_future<bool> meta_future; // 附件元数据持久化返回的 future，可拷贝
    mail() = default;
    mail(const mail& other) {
        id = other.id;
        ids = other.ids;
        from = other.from;
        to = other.to;
        header = other.header;
        body = other.body;
        subject = other.subject;
        source_message_id = other.source_message_id;
        send_time = other.send_time;
        box_id = other.box_id;
        status = other.status;
        attachments = other.attachments;
        persist_status = other.persist_status;
        body_path = other.body_path;
        mail_over = other.mail_over;
        deduplicated_inbound = other.deduplicated_inbound;
    }
    mail(mail&& other) noexcept {
        id = other.id;
        ids = std::move(other.ids);
        from = std::move(other.from);
        to = std::move(other.to);
        header = std::move(other.header);
        body = std::move(other.body);
        subject = std::move(other.subject);
        source_message_id = std::move(other.source_message_id);
        send_time = other.send_time;
        box_id = other.box_id;
        status = other.status;
        attachments = std::move(other.attachments);
        persist_status = other.persist_status;
        body_path = std::move(other.body_path);
        mail_over = other.mail_over;
        deduplicated_inbound = other.deduplicated_inbound;
    }
};
