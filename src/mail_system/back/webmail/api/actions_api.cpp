#include "mail_system/back/webmail/api/actions_api.h"
#include "mail_system/back/webmail/api/api_util.h"
#include "mail_system/back/common/safe_parse.h"
#include <algorithm>
#include <cctype>
#include <vector>

namespace mail_system::webmail::api {
namespace {
std::string encoded_filename(const std::string& name) {
    const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : name) {
        if ((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='.' || c=='-' || c=='_') out += char(c);
        else { out += '%'; out += hex[c>>4]; out += hex[c&15]; }
    }
    return out.empty() ? "attachment" : out;
}
}
bool route_actions(const pr::http::HttpRequest& req, SessionStore& sessions, MailActions* actions, const Complete& complete) {
    if (req.path.rfind("/api/mailboxes/", 0) != 0) return false;
    std::vector<std::string> parts;
    size_t pos = 1;
    while (pos <= req.path.size()) {
        auto end = req.path.find('/', pos);
        parts.push_back(req.path.substr(pos, end == std::string::npos ? end : end - pos));
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    bool patch = parts.size() == 5 && req.method == "PATCH";
    bool download = parts.size() == 7 && parts[5] == "attachments";
    if ((!patch && !download) || parts[1] != "mailboxes" || parts[3] != "messages") return false;
    uint64_t box = 0, mail = 0, attachment = 0;
    if (!parse_uint(parts[2], box) || !parse_uint(parts[4], mail) || (download && !parse_uint(parts[6], attachment))) {
        complete(error_response(404, "not found")); return true;
    }
    if (download && req.method != "GET" && req.method != "HEAD") { complete(error_response(405, "method not allowed")); return true; }
    if (!actions) { complete(error_response(503, "mail actions unavailable")); return true; }
    sessions.lookup_async(SessionStore::token_from_request(req), [req, actions, complete, box, mail, attachment, download](SessionStore::LookupResult session) {
        if (session.server_error) { complete(error_response(503, "session store unavailable")); return; }
        if (!session.found) { complete(error_response(401, "not authenticated")); return; }
        if (download) {
            actions->download_async(session.user.user_id, session.user.address, box, mail, attachment,
                [complete, head = req.method == "HEAD"](AttachmentDownload out) {
                    if (out.server_error) { complete(error_response(503, "attachment storage unavailable")); return; }
                    if (out.not_found) { complete(error_response(404, "attachment not found")); return; }
                    pr::http::HttpResponse response;
                    response.content_type = "application/octet-stream";
                    response.content_length = static_cast<int64_t>(out.source->size());
                    if (!head) response.body_source = std::move(out.source);
                    response.extra_headers = "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
                        "Content-Disposition: attachment; filename=\"attachment\"; filename*=UTF-8''" + encoded_filename(out.filename) + "\r\n";
                    complete(std::move(response));
                });
            return;
        }
        auto type = req.find_header("content-type");
        std::string media = type ? type->substr(0, type->find(';')) : "";
        std::transform(media.begin(), media.end(), media.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (media != "application/json") { complete(error_response(415, "expected application/json")); return; }
        auto body = nlohmann::json::parse(req.body, nullptr, false);
        if (!body.is_object() || body.empty() || body.size() > 2) { complete(error_response(400, "expected read/is_starred booleans")); return; }
        std::optional<bool> read, starred;
        for (auto it = body.begin(); it != body.end(); ++it) {
            if (!it->is_boolean() || (it.key() != "read" && it.key() != "is_starred")) { complete(error_response(400, "unsupported message flag")); return; }
            if (it.key() == "read") read = it->get<bool>(); else starred = it->get<bool>();
        }
        actions->update_async(session.user.user_id, session.user.address, box, mail, read, starred, [complete](MailActionResult out) {
            if (out.server_error) complete(error_response(503, "message update unavailable"));
            else if (out.not_found) complete(error_response(404, "message not found"));
            else complete(json_response(200, {{"ok", true}}));
        });
    });
    return true;
}
}
