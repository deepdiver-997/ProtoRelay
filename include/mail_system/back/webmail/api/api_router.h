#ifndef MAIL_SYSTEM_WEBMAIL_API_API_ROUTER_H
#define MAIL_SYSTEM_WEBMAIL_API_API_ROUTER_H

// ================================================================
// ApiRouter — /api/* 路由表（webmail_server 与单测共用的派发表）
//
// 认领式：返回 false = 不是 API 请求（落静态文件）；返回 true = 已认领，
// 响应经 complete 恰好回吐一次（同步端点在调用线程内 complete，异步端点
// 在 worker 线程 complete —— session 写经 exec_ctx_ 串行化，均安全）。
// 不依赖 server 对象，单测可直接构造驱动。
// ================================================================

#include "framework/http/http_types.hpp"
#include "mail_system/back/webmail/api/api_util.h"
#include "mail_system/back/webmail/api/auth_api.h"

#include <functional>
#include <string>

namespace mail_system::webmail {

class ApiRouter {
public:
    ApiRouter(AuthService& auth, SessionStore& sessions)
        : auth_(auth), sessions_(sessions) {}

    using Complete = std::function<void(pr::http::HttpResponse)>;

    bool handle(const pr::http::HttpRequest& req, const Complete& complete) const {
        // 只认领 /api 前缀；其余一切（含 /api 精确路径）归静态文件管线
        if (req.path.rfind("/api/", 0) != 0) return false;

        if (req.path == "/api/health") {
            if (req.method != "GET" && req.method != "HEAD") {
                complete(api::error_response(405, "method not allowed"));
                return true;
            }
            complete(api::health());
            return true;
        }
        if (req.path == "/api/login") {
            if (req.method != "POST") {
                complete(api::error_response(405, "method not allowed"));
                return true;
            }
            api::login(req, auth_, sessions_, complete);
            return true;
        }
        if (req.path == "/api/logout") {
            if (req.method != "POST") {
                complete(api::error_response(405, "method not allowed"));
                return true;
            }
            complete(api::logout(req, sessions_));
            return true;
        }
        if (req.path == "/api/me") {
            if (req.method != "GET") {
                complete(api::error_response(405, "method not allowed"));
                return true;
            }
            complete(api::me(req, sessions_));
            return true;
        }
        complete(api::error_response(404, "not found"));
        return true;
    }

private:
    AuthService& auth_;
    SessionStore& sessions_;
};

} // namespace mail_system::webmail

#endif // MAIL_SYSTEM_WEBMAIL_API_API_ROUTER_H
