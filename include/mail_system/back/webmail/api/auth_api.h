#ifndef MAIL_SYSTEM_WEBMAIL_API_AUTH_API_H
#define MAIL_SYSTEM_WEBMAIL_API_AUTH_API_H

// ================================================================
// auth_api — 登录/会话端点（webmail MVP 第一步，kickoff §三）
//
//   GET  /api/health  存活探针（同步、零依赖——DB/存储全挂也必须 200）
//   POST /api/login   登录：AuthService（三协议同口径验密）→ 发 cookie
//   POST /api/logout  注销：删会话 + 清 cookie（幂等，无 cookie 也 200）
//   GET  /api/me      会话自省：未登录 401
// ================================================================

#include "framework/http/http_types.hpp"
#include "mail_system/back/webmail/auth_service.h"
#include "mail_system/back/webmail/session_store.h"

#include <functional>

namespace mail_system::webmail::api {

using Complete = std::function<void(pr::http::HttpResponse)>;

// 同步端点：直接返回响应（session 的 complete 由路由层调用）
pr::http::HttpResponse health();
pr::http::HttpResponse logout(const pr::http::HttpRequest& req, SessionStore& sessions);
pr::http::HttpResponse me(const pr::http::HttpRequest& req, SessionStore& sessions);

// 异步端点：认证在 worker 线程跑（bcrypt），完成后经 complete 回吐。
// complete 恰好调用一次；回调线程 = worker 线程（响应写经 exec_ctx_ 串行化）。
void login(const pr::http::HttpRequest& req, AuthService& auth, SessionStore& sessions,
           const Complete& complete);

} // namespace mail_system::webmail::api

#endif // MAIL_SYSTEM_WEBMAIL_API_AUTH_API_H
