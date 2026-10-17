#ifndef MAIL_SYSTEM_WEBMAIL_API_MAIL_API_H
#define MAIL_SYSTEM_WEBMAIL_API_MAIL_API_H

// ================================================================
// mail_api — 邮件读路径端点（webmail MVP-2 第一步：列表/分页）
//
//   GET /api/mailboxes                    邮箱列表（含每箱 total/unread）
//   GET /api/mailboxes/{id}/messages      单箱邮件分页（?page=&page_size=）
//
// 两个端点都要求会话（cookie）；会话校验在内存表，直连路径不做 DB 往返。
// DB 查询经 MailService 异步编排，complete 恰好调用一次。
// ================================================================

#include "framework/http/http_types.hpp"
#include "mail_system/back/webmail/mail_service.h"
#include "mail_system/back/webmail/session_store.h"

#include <cstdint>
#include <functional>

namespace mail_system::webmail::api {

using Complete = std::function<void(pr::http::HttpResponse)>;

void list_mailboxes(const pr::http::HttpRequest& req, SessionStore& sessions,
                    MailService& mail, const Complete& complete);

// mailbox_id 已由路由层严格 parse（非数字不会到达这里）
void list_messages(const pr::http::HttpRequest& req, SessionStore& sessions,
                   MailService& mail, uint64_t mailbox_id, const Complete& complete);

} // namespace mail_system::webmail::api

#endif // MAIL_SYSTEM_WEBMAIL_API_MAIL_API_H
