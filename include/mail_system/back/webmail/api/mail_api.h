#ifndef MAIL_SYSTEM_WEBMAIL_API_MAIL_API_H
#define MAIL_SYSTEM_WEBMAIL_API_MAIL_API_H

// ================================================================
// mail_api — 邮件读路径端点（webmail MVP-2）
//
//   GET /api/mailboxes                        邮箱列表（含每箱 total/unread）
//   GET /api/mailboxes/{id}/messages          单箱邮件分页（?page=&page_size=）
//   GET /api/mailboxes/{id}/messages/{mid}    读信（解码正文 + 附件清单）
//
// 读信返回的 html 字段已经 html_sanitize 白名单清洗；红线要求前端仍以
// sandbox iframe（无 same-origin/脚本）+ CSP 渲染——两层防线缺一不可。
// 所有端点要求会话（cookie）；DB 查询经 MailService 异步编排，
// complete 恰好调用一次。
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

void get_message(const pr::http::HttpRequest& req, SessionStore& sessions,
                 MailService& mail, uint64_t mailbox_id, uint64_t mail_id,
                 const Complete& complete);

} // namespace mail_system::webmail::api

#endif // MAIL_SYSTEM_WEBMAIL_API_MAIL_API_H
