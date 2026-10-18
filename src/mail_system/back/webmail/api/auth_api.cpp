#include "mail_system/back/webmail/api/auth_api.h"
#include "mail_system/back/webmail/api/api_util.h"

#include <nlohmann/json.hpp>

namespace mail_system::webmail::api {

pr::http::HttpResponse health() {
    return json_response(200, nlohmann::json{{"status", "ok"}});
}

void logout(const pr::http::HttpRequest& req, SessionStore& sessions, const Complete& complete) {
    sessions.erase_async(SessionStore::token_from_request(req), [complete](bool ok) {
        if (!ok) { complete(error_response(503, "session store unavailable")); return; }
        complete(json_response(200, {{"ok", true}}, SessionStore::clear_cookie_header()));
    });
}

void me(const pr::http::HttpRequest& req, SessionStore& sessions, const Complete& complete) {
    sessions.lookup_async(SessionStore::token_from_request(req), [complete](SessionStore::LookupResult r) {
        if (r.server_error) { complete(error_response(503, "session store unavailable")); return; }
        if (!r.found) { complete(error_response(401, "not authenticated")); return; }
        complete(json_response(200, {{"user", {{"id", r.user.user_id}, {"address", r.user.address}}}}));
    });
}

void login(const pr::http::HttpRequest& req, AuthService& auth, SessionStore& sessions,
           const Complete& complete) {
    // body 解析：只认 application/json 的扁平对象 {"address":..., "password":...}
    nlohmann::json body = nlohmann::json::parse(req.body, nullptr, /*allow_exceptions=*/false);
    if (body.is_discarded() || !body.is_object() ||
        !body.contains("address") || !body.contains("password") ||
        !body["address"].is_string() || !body["password"].is_string()) {
        complete(error_response(400, "expected JSON body {address, password}"));
        return;
    }
    auto address = body["address"].get<std::string>();
    auto password = body["password"].get<std::string>();
    if (address.empty() || password.empty()) {
        complete(error_response(400, "address and password required"));
        return;
    }

    // Production DB sessions never retain the SMTP password.
    auto submission_password = sessions.database_backed() ? nullptr : std::make_shared<SessionPassword>(password);
    auth.login_async(address, password,
        [&sessions, complete, submission_password](AuthOutcome out) {
            if (out.server_error) {
                complete(error_response(503, "database unavailable"));
                return;
            }
            if (!out.ok) {
                // 与三协议同口径：不区分账号不存在/密码错/已禁用，统一 401
                complete(error_response(401, "invalid credentials"));
                return;
            }
            std::string token = SessionStore::generate_token();
            if (token.empty()) {
                complete(error_response(500, "token generation failed"));
                return;
            }
            sessions.put_async(token, {out.user_id, out.address, out.credential_stamp, out.shard}, submission_password,
                [&sessions, token, out, complete](bool ok) {
                    if (!ok) { complete(error_response(503, "session store unavailable")); return; }
                    complete(json_response(200, {{"user", {{"id", out.user_id}, {"address", out.address}}}},
                        sessions.issue_cookie_header(token)));
                });
        });
}

} // namespace mail_system::webmail::api
