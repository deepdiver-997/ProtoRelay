#ifndef MAIL_SYSTEM_WEBMAIL_API_API_UTIL_H
#define MAIL_SYSTEM_WEBMAIL_API_API_UTIL_H

// webmail API 公共小工具：JSON 响应组装 / 状态文案。全 inline，无状态。

#include "framework/http/http_types.hpp"
#include <nlohmann/json.hpp>

namespace mail_system::webmail::api {

inline const char* status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default:  return "Internal Server Error";
    }
}

// JSON 响应统一出口：content_type=application/json；extra_headers 逐条自带
// CRLF（Set-Cookie 等）。nlohmann dump 保证转义合法，不手拼字符串。
inline pr::http::HttpResponse json_response(int status, const nlohmann::json& body,
                                            std::string extra_headers = {}) {
    pr::http::HttpResponse r;
    r.status = status;
    r.status_text = status_text(status);
    r.content_type = "application/json";
    r.body = body.dump();
    r.extra_headers = std::move(extra_headers);
    return r;
}

inline pr::http::HttpResponse error_response(int status, const char* message,
                                             std::string extra_headers = {}) {
    return json_response(status, nlohmann::json{{"error", message}},
                         std::move(extra_headers));
}

} // namespace mail_system::webmail::api

#endif // MAIL_SYSTEM_WEBMAIL_API_API_UTIL_H
