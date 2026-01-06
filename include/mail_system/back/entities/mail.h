#pragma once

#include<iostream>
#include<string>
#include<vector>
#include<ctime>
#include <fstream>

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
};

struct mail
{
    size_t id;                  // 主键
    std::string from;           // 发件人邮箱地址
    std::vector<std::string> to;// 收件人邮箱地址
    std::string header;         // 邮件头
    std::string body;           // 邮件正文
    std::string subject;        // 邮件主题
    time_t send_time;           // 发送时间
    int box_id;                 // 邮件所属邮箱ID
    int status;                 // 邮件状态：0已读，1未读，2未送达，3草稿，4垃圾邮件，5已删除
    std::string body_path;      // 邮件正文存储路径（使用文件存储）
    std::vector<attachment> attachments; // 附件元数据列表
    mail() = default;
    mail(size_t id, std::string from, std::vector<std::string> to, std::string header, std::string body, time_t send_time, int box_id, int status)
        : id(id), from(from), to(to), header(header), body(body), send_time(send_time), box_id(box_id), status(status) {}
    mail(const mail& other) {
        id = other.id;
        from = other.from;
        to = other.to;
        header = other.header;
        body = other.body;
        subject = other.subject;
        send_time = other.send_time;
        box_id = other.box_id;
        status = other.status;
        attachments = other.attachments;
    }
    mail(mail&& other) noexcept {
        id = other.id;
        from = std::move(other.from);
        to = std::move(other.to);
        header = std::move(other.header);
        body = std::move(other.body);
        subject = std::move(other.subject);
        send_time = other.send_time;
        box_id = other.box_id;
        status = other.status;
        attachments = std::move(other.attachments);
    }
    static bool save_mail_to_file(const mail& mail, const std::string& file_path) {
        std::ofstream out(file_path);
        if (out.is_open()) {
            out << mail.body;
            out.close();
            return true;
        }
        return false;
    }
};