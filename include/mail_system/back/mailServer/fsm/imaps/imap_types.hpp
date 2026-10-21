#ifndef IMAPS_FSM_H
#define IMAPS_FSM_H

#include "mail_system/back/entities/mailbox.h"
#include <cstdint>
#include <string>
#include <ctime>
#include "mail_system/back/common/safe_parse.h"   // safe_stoull（曾在此处定义，2026-10-17 收口）

namespace mail_system {

// ====================================================================
// IMAP 状态枚举 (RFC 3501 §3 基本状态)
// ====================================================================
enum class ImapState {
    INIT = 0,                // 连接刚建立
    NOT_AUTHENTICATED = 1,   // 等待 LOGIN / AUTHENTICATE
    AUTHENTICATED = 2,       // 已登录，无选中的邮箱
    SELECTED = 3,            // 已选中一个邮箱
    LOGOUT = 4,              // LOGOUT 发送完毕
    CLOSED = 5,              // 连接关闭
    COUNT = 6
};

// ====================================================================
// IMAP 事件枚举 —— 等价于命令名映射
// ====================================================================
enum class ImapEvent {
    CONNECT = 0,
    CAPABILITY = 1,
    LOGIN = 2,
    AUTHENTICATE = 3,
    LOGOUT = 4,
    SELECT = 5,
    EXAMINE = 6,
    CREATE = 7,
    DELETE = 8,
    RENAME = 9,
    SUBSCRIBE = 10,
    UNSUBSCRIBE = 11,
    LIST = 12,
    LSUB = 13,
    IMAP_STATUS = 14,   // 避免与 MySQL headers 中的 STATUS 宏冲突
    APPEND = 15,
    CHECK = 16,
    CLOSE = 17,
    EXPUNGE = 18,
    SEARCH = 19,
    FETCH = 20,
    STORE = 21,
    COPY = 22,
    MOVE = 23,
    UID = 24,
    NOOP = 25,
    IDLE = 26,
    DONE = 27,
    STARTTLS = 28,
    ERROR = 29,
    TIMEOUT = 30,
    COUNT = 31
};

// ====================================================================
// IMAP 上下文 —— 存储每次会话的状态
// ====================================================================
struct ImapContext {
    // 认证
    bool is_authenticated = false;
    std::string username;                     // 登录名（邮箱地址）
    uint64_t user_id = 0;                     // users.id
    int shard_index = 0;                     // 由 shard router 在认证时分配

    // 命令标签
    std::string current_tag;                  // 当前命令的 tag，响应时回显

    // 选中的邮箱
    bool mailbox_selected = false;
    std::string selected_mailbox_name;        // 邮箱名称，如 "INBOX"
    uint64_t selected_mailbox_id = 0;         // mailboxes.id
    uint64_t uid_validity = 0;               // UIDVALIDITY 值

    // 读写模式
    bool read_only = false;                   // true = EXAMINE, false = SELECT

    // IDLE
    bool idle_mode = false;                   // 是否处于 IDLE 状态

    // SASL PLAIN（AUTHENTICATE）：0=非进行中 1=已发 "+ " 等待续行 2=续行已到待 FSM 消费。
    // 续行（base64 响应或 "*" 中止）不是 IMAP 命令，由 session handle_read 拦截后
    // 以 AUTHENTICATE 事件回到 FSM；tag 沿用命令行的（current_tag 未被覆盖）。
    int sasl_plain_state = 0;
    std::string sasl_plain_response;          // 续行原文

    // UID 命令标记（handle_uid 设置，handle_fetch/search 读取）
    bool is_uid_command = false;
    std::string uid_overridden_args;          // UID→seq 转换后的命令参数

    // APPEND 文字量等待
    bool awaiting_literal = false;            // 等待 APPEND 文字量数据
    size_t literal_size = 0;                  // 期望的文字量字节数
    std::string literal_buffer;               // 已接收的文字量数据
    std::string pending_append_mailbox;       // APPEND 的目标邮箱
    std::string pending_append_flags;         // APPEND 的 flags
    std::string pending_append_internaldate;  // APPEND 的 InternalDate
    std::string pending_append_preamble;      // APPEND 原始参数（literal 前的部分）

    void clear() {
        is_authenticated = false;
        username.clear();
        user_id = 0;
        shard_index = 0;
        current_tag.clear();
        mailbox_selected = false;
        selected_mailbox_name.clear();
        selected_mailbox_id = 0;
        uid_validity = 0;
        read_only = false;
        idle_mode = false;
        sasl_plain_state = 0;
        sasl_plain_response.clear();
        awaiting_literal = false;
        literal_size = 0;
        literal_buffer.clear();
        pending_append_mailbox.clear();
        pending_append_flags.clear();
        pending_append_internaldate.clear();
        pending_append_preamble.clear();
    }
};


} // namespace mail_system

#endif // IMAPS_FSM_H
