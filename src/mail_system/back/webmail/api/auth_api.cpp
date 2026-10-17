#include "mail_system/back/webmail/api/auth_api.h"
#include "mail_system/back/webmail/api/api_util.h"

#include <nlohmann/json.hpp>

namespace mail_system::webmail::api {

pr::http::HttpResponse health() {
    return json_response(200, nlohmann::json{{"status", "ok"}});
}

pr::http::HttpResponse logout(const pr::http::HttpRequest& req, SessionStore& sessions) {
    sessions.erase(SessionStore::token_from_request(req));
    return json_response(200, nlohmann::json{{"ok", true}},
                         SessionStore::clear_cookie_header());
}

pr::http::HttpResponse me(const pr::http::HttpRequest& req, SessionStore& sessions) {
    WebSessionInfo info;
    const std::string token = SessionStore::token_from_request(req);
    if (token.empty() || !sessions.lookup(token, info))
        return error_response(401, "not authenticated");
    return json_response(200, nlohmann::json{
        {"user", {{"id", info.user_id}, {"address", info.address}}}});
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

    auth.login_async(address, password,
        [&sessions, complete](AuthOutcome out) {
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
            sessions.put(token, out.user_id, out.address);
            complete(json_response(200, nlohmann::json{
                {"user", {{"id", out.user_id}, {"address", out.address}}}},
                sessions.issue_cookie_header(token)));
        });
}

} // namespace mail_system::webmail::api
