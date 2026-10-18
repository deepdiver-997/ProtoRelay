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
#include "mail_system/back/common/safe_parse.h"
#include "mail_system/back/webmail/api/api_util.h"
#include "mail_system/back/webmail/api/auth_api.h"
#include "mail_system/back/webmail/api/mail_api.h"
#include "mail_system/back/webmail/api/compose_api.h"
#include "mail_system/back/webmail/mail_service.h"

#include <cstring>
#include <functional>
#include <string>

namespace mail_system::webmail {

class ApiRouter {
public:
    ApiRouter(AuthService& auth, SessionStore& sessions, MailService& mail,
              ISmtpSubmitter* submitter = nullptr)
        : auth_(auth), sessions_(sessions), mail_(mail), submitter_(submitter) {}

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
        if (req.path == "/api/send") {
            if (req.method != "POST") {
                complete(api::error_response(405, "method not allowed"));
            } else if (!submitter_) {
                complete(api::error_response(503, "submission service unavailable"));
            } else {
                api::send_message(req, sessions_, *submitter_, complete);
            }
            return true;
        }
        if (req.path == "/api/mailboxes") {
            if (req.method != "GET") {
                complete(api::error_response(405, "method not allowed"));
                return true;
            }
            api::list_mailboxes(req, sessions_, mail_, complete);
            return true;
        }
        // /api/mailboxes/{id}/messages —— id 严格 parse，非数字按未知路径 404
        static constexpr char kMbPrefix[] = "/api/mailboxes/";
        static constexpr char kMsgSuffix[] = "/messages";
        if (req.path.rfind(kMbPrefix, 0) == 0 &&
            req.path.size() > strlen(kMbPrefix) + strlen(kMsgSuffix) &&
            req.path.compare(req.path.size() - strlen(kMsgSuffix),
                             strlen(kMsgSuffix), kMsgSuffix) == 0) {
            const std::string id_str = req.path.substr(
                strlen(kMbPrefix),
                req.path.size() - strlen(kMbPrefix) - strlen(kMsgSuffix));
            uint64_t mailbox_id = 0;
            if (!parse_uint(id_str, mailbox_id)) {
                complete(api::error_response(404, "not found"));
                return true;
            }
            if (req.method != "GET") {
                complete(api::error_response(405, "method not allowed"));
                return true;
            }
            api::list_messages(req, sessions_, mail_, mailbox_id, complete);
            return true;
        }
        // /api/mailboxes/{mb}/messages/{mail} —— 读信（两级 id 都严格 parse）
        {
            const std::string& path = req.path;
            if (path.rfind(kMbPrefix, 0) == 0) {
                const size_t mid = path.find("/messages/", strlen(kMbPrefix));
                if (mid != std::string::npos) {
                    const std::string mb_str = path.substr(strlen(kMbPrefix),
                                                           mid - strlen(kMbPrefix));
                    const std::string mail_str = path.substr(mid + strlen("/messages/"));
                    uint64_t mailbox_id = 0, mail_id = 0;
                    // 任何一段非数字（含空段）→ 未知路径 404
                    if (!mb_str.empty() && !mail_str.empty() &&
                        parse_uint(mb_str, mailbox_id) && parse_uint(mail_str, mail_id)) {
                        if (req.method != "GET") {
                            complete(api::error_response(405, "method not allowed"));
                            return true;
                        }
                        api::get_message(req, sessions_, mail_, mailbox_id, mail_id,
                                         complete);
                        return true;
                    }
                    complete(api::error_response(404, "not found"));
                    return true;
                }
            }
        }
        complete(api::error_response(404, "not found"));
        return true;
    }

private:
    AuthService& auth_;
    SessionStore& sessions_;
    MailService& mail_;
    ISmtpSubmitter* submitter_;
};

} // namespace mail_system::webmail

#endif // MAIL_SYSTEM_WEBMAIL_API_API_ROUTER_H
