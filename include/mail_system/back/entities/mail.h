#pragma once

#include<iostream>
#include<string>
#include<ctime>

struct mail
{
    size_t id;                  // 主键
    std::string from;           // 发件人邮箱地址
    std::vector<std::string> to;// 收件人邮箱地址
    std::string header;        // 邮件头
    std::string body;           // 邮件正文
    time_t send_time;          // 发送时间
    int box_id;                // 邮件所属邮箱ID
    bool is_draft;             // 是否为草稿
    bool is_read;              // 是否已读
    mail() = default;
    mail(const mail& other) {
        id = other.id;
        from = other.from;
        to = other.to;
        header = other.header;
        body = other.body;
        send_time = other.send_time;
        box_id = other.box_id;
        is_draft = other.is_draft;
        is_read = other.is_read;
    }
    mail(mail&& other) noexcept {
        id = other.id;
        from = std::move(other.from);
        to = std::move(other.to);
        header = std::move(other.header);
        body = std::move(other.body);
        send_time = other.send_time;
        box_id = other.box_id;
        is_draft = other.is_draft;
        is_read = other.is_read;
    }
};

struct mailbox
{
    size_t id;                  // 主键
    size_t user_id;            // 所属用户ID
    std::string name;          // 邮箱名称
    bool is_system;            // 是否为系统默认邮箱（收件箱、发件箱、垃圾箱等）
    int box_type;              // 系统邮箱类型：1收件箱，2发件箱，3垃圾箱，4已删除
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
};