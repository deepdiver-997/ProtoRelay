#include "mail_system/back/webmail/api/mail_api.h"
#include "mail_system/back/webmail/api/api_util.h"
#include "mail_system/back/webmail/html_sanitize.h"

#include <nlohmann/json.hpp>

namespace mail_system::webmail::api {

namespace {

// 会话校验：无 cookie / token 未知或过期 → false（上层 401，不区分原因）
bool session_user(const SessionStore& sessions, const pr::http::HttpRequest& req,
                  WebSessionInfo& out) {
    const std::string token = SessionStore::token_from_request(req);
    return !token.empty() && sessions.lookup(token, out);
}

// query string 取单个参数（"a=1&b=2"；不做 URL 解码——参数全是数字，编码
// 后必然 parse 失败走 400，无需解码面）
std::string query_param(const std::string& query, const std::string& key) {
    size_t pos = 0;
    while (pos < query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        const std::string pair = query.substr(pos, amp - pos);
        const size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == key)
            return pair.substr(eq + 1);
        pos = amp + 1;
    }
    return {};
}

nlohmann::json mailbox_json(const MailboxSummary& mb) {
    return {{"id", mb.id}, {"name", mb.name}, {"box_type", mb.box_type},
            {"total", mb.total}, {"unread", mb.unread}};
}

nlohmann::json mail_item_json(const MailListItem& m) {
    return {{"id", m.id},
            {"subject", mime_view::decode_header(m.subject)},
            {"sender", m.sender},
            {"recipients", m.recipients},
            {"send_time", static_cast<int64_t>(m.send_time)},
            {"status", m.status},
            {"is_starred", m.starred},
            {"is_important", m.important},
            {"is_deleted", m.deleted},
            {"has_attachment", m.has_attachment}};
}

} // namespace

void list_mailboxes(const pr::http::HttpRequest& req, SessionStore& sessions,
                    MailService& mail, const Complete& complete) {
    WebSessionInfo info;
    if (!session_user(sessions, req, info)) {
        complete(error_response(401, "not authenticated"));
        return;
    }
    mail.list_mailboxes_async(info.user_id, info.address,
        [complete](MailboxListResult out) {
            if (out.server_error) {
                complete(error_response(503, "database unavailable"));
                return;
            }
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& mb : out.mailboxes) arr.push_back(mailbox_json(mb));
            complete(json_response(200, nlohmann::json{{"mailboxes", std::move(arr)}}));
        });
}

void list_messages(const pr::http::HttpRequest& req, SessionStore& sessions,
                   MailService& mail, uint64_t mailbox_id, const Complete& complete) {
    WebSessionInfo info;
    if (!session_user(sessions, req, info)) {
        complete(error_response(401, "not authenticated"));
        return;
    }

    // 分页参数：缺省 page=1 / page_size=50；出现即必须是无符号数字
    //（"0"、">100" 由 service 钳制，非数字是客户端 bug → 400）
    uint32_t page = 1;
    if (auto v = query_param(req.query, "page"); !v.empty()) {
        uint64_t parsed = 0;
        if (!parse_uint(v, parsed) || parsed == 0) {
            complete(error_response(400, "invalid page"));
            return;
        }
        page = static_cast<uint32_t>(parsed);
    }
    uint32_t page_size = 50;
    if (auto v = query_param(req.query, "page_size"); !v.empty()) {
        uint64_t parsed = 0;
        if (!parse_uint(v, parsed) || parsed == 0) {
            complete(error_response(400, "invalid page_size"));
            return;
        }
        page_size = static_cast<uint32_t>(parsed);
    }

    mail.list_messages_async(info.user_id, info.address, mailbox_id, page, page_size,
        [complete](MessagePageResult out) {
            if (out.server_error) {
                complete(error_response(503, "database unavailable"));
                return;
            }
            if (out.not_found) {
                // 不区分"无此箱/非你的箱"，跨用户探测只得到 404
                complete(error_response(404, "mailbox not found"));
                return;
            }
            const uint64_t total_pages = out.total == 0
                ? 0 : (out.total + out.page_size - 1) / out.page_size;
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& m : out.messages) arr.push_back(mail_item_json(m));
            complete(json_response(200, nlohmann::json{
                {"mailbox", {{"id", out.mailbox.id}, {"name", out.mailbox.name},
                             {"box_type", out.mailbox.box_type}}},
                {"total", out.total},
                {"page", out.page},
                {"page_size", out.page_size},
                {"total_pages", total_pages},
                {"messages", std::move(arr)}}));
        });
}

void get_message(const pr::http::HttpRequest& req, SessionStore& sessions,
                 MailService& mail, uint64_t mailbox_id, uint64_t mail_id,
                 const Complete& complete) {
    WebSessionInfo info;
    if (!session_user(sessions, req, info)) {
        complete(error_response(401, "not authenticated"));
        return;
    }
    mail.get_message_async(info.user_id, info.address, mailbox_id, mail_id,
        [complete](MessageDetail out) {
            if (out.server_error) {
                complete(error_response(503, "database unavailable"));
                return;
            }
            if (out.not_found) {
                // 不区分"无此信/不在此箱/非你的信"，跨用户探测只得到 404
                complete(error_response(404, "message not found"));
                return;
            }
            nlohmann::json atts = nlohmann::json::array();
            for (const auto& a : out.attachments)
                atts.push_back({{"id", a.id}, {"filename", a.filename},
                                {"size", a.size}, {"mime_type", a.mime_type}});
            nlohmann::json body = nlohmann::json{
                {"text_available", out.text_available},
                {"html_available", out.html_available},
                {"charset_text", out.charset_text},
                {"charset_html", out.charset_html},
                {"text_truncated", out.text_truncated},
                {"html_truncated", out.html_truncated},
            };
            if (out.text_available) body["text"] = std::move(out.text);
            if (out.html_available) {
                // 白名单清洗（kickoff 设计决策 5 第一道防线）；前端仍须
                // sandbox iframe + CSP 渲染——两层防线缺一不可
                body["html"] = html_sanitize::sanitize(std::move(out.html));
            }
            complete(json_response(200, nlohmann::json{
                {"message", {{"id", out.id},
                             {"subject", mime_view::decode_header(out.subject)},
                             {"sender", out.sender},
                             {"recipients", out.recipients},
                             {"send_time", static_cast<int64_t>(out.send_time)},
                             {"status", out.status},
                             {"is_starred", out.starred},
                             {"is_important", out.important},
                             {"is_deleted", out.deleted}}},
                {"body", std::move(body)},
                {"body_available", out.body_available},
                {"attachments", std::move(atts)}}));
        });
}

} // namespace mail_system::webmail::api
