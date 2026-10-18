#ifndef MAIL_SYSTEM_WEBMAIL_API_API_UTIL_H
#define MAIL_SYSTEM_WEBMAIL_API_API_UTIL_H

// webmail API 公共小工具：JSON 响应组装 / 状态文案。全 inline，无状态。

#include "framework/http/http_types.hpp"
#include <nlohmann/json.hpp>

namespace mail_system::webmail::api {

inline const char* status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 415: return "Unsupported Media Type";
        case 422: return "Unprocessable Content";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default:  return "Internal Server Error";
    }
}

// JSON 响应统一出口：content_type=application/json；extra_headers 逐条自带
// CRLF（Set-Cookie 等）。dump 带 replace 错误处理器：邮件正文可能携带非
// UTF-8 字节（charset 声明缺失/脏数据），默认 dump 会抛 316 异常打穿回调
// 线程——replace 把非法字节替换为 U+FFFD，响应永远可序列化。
inline void stringify_ids(nlohmann::json& value) {
    if (value.is_array()) { for (auto& item : value) stringify_ids(item); }
    else if (value.is_object()) for (auto it = value.begin(); it != value.end(); ++it) {
        if (it.key() == "id" && it->is_number_integer()) *it = std::to_string(it->get<uint64_t>());
        else stringify_ids(*it);
    }
}
inline pr::http::HttpResponse json_response(int status, nlohmann::json body,
                                            std::string extra_headers = {}) {
    pr::http::HttpResponse r;
    r.status = status;
    r.status_text = status_text(status);
    r.content_type = "application/json";
    stringify_ids(body);
    r.body = body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    r.extra_headers = "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n" + extra_headers;
    return r;
}

inline pr::http::HttpResponse error_response(int status, const char* message,
                                             std::string extra_headers = {}) {
    return json_response(status, nlohmann::json{{"error", message}},
                         std::move(extra_headers));
}

} // namespace mail_system::webmail::api

#endif // MAIL_SYSTEM_WEBMAIL_API_API_UTIL_H
