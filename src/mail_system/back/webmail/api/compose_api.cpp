#include "mail_system/back/webmail/api/compose_api.h"
#include "mail_system/back/webmail/api/api_util.h"
#include "mail_system/back/common/logger.h"
#include "mail_system/back/common/mime_composer.h"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>

namespace mail_system::webmail::api {

void send_message(const pr::http::HttpRequest& req, SessionStore& sessions,
                  ISmtpSubmitter& submitter, const Complete& complete) {
    const auto token = SessionStore::token_from_request(req);
    sessions.lookup_async(token, [req, &sessions, &submitter, complete, token](SessionStore::LookupResult session) {
        if (session.server_error) { complete(error_response(503, "session store unavailable")); return; }
        if (!session.found) { complete(error_response(401, "login required for submission")); return; }
        const auto& user = session.user;
        auto password = session.password;
        const auto* type = req.find_header("content-type");
        std::string media_type = type ? type->substr(0, type->find(';')) : "";
        std::transform(media_type.begin(), media_type.end(), media_type.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        while (!media_type.empty() && media_type.back() == ' ') media_type.pop_back();
        if (media_type != "application/json") {
            complete(error_response(415, "expected application/json")); return;
        }
        if (req.body.size() > 1024 * 1024) {
            complete(error_response(413, "request exceeds 1 MiB")); return;
        }
        auto body = nlohmann::json::parse(req.body, nullptr, false);
        if (!body.is_object()) { complete(error_response(400, "expected JSON object")); return; }
        mime::Message message;
        message.from = user.address; // 唯一来源：已认证会话。请求不能选择发件人。
        for (auto it = body.begin(); it != body.end(); ++it) {
            const auto& key = it.key();
            if (key != "to" && key != "cc" && key != "subject" && key != "text" && key != "html") {
                complete(error_response(400, "unsupported message field")); return;
            }
            if (key == "to" || key == "cc") {
                if (!it->is_array() || it->size() > mime::kMaxRecipients) {
                    complete(error_response(400, "recipients must be arrays of at most 100 addresses")); return;
                }
                auto& list = key == "to" ? message.to : message.cc;
                for (const auto& entry : *it) {
                    if (!entry.is_string()) { complete(error_response(400, "recipient must be a string")); return; }
                    list.push_back(entry.get<std::string>());
                }
            } else {
                if (!it->is_string()) { complete(error_response(400, "message fields must be strings")); return; }
                const auto value = it->get<std::string>();
                if (key == "subject") message.subject = value;
                else if (key == "text") message.text = value;
                else message.html = value;
            }
        }
        auto composed = mime::compose(message);
        if (composed.error_status) {
            if (composed.error_status >= 500)
                LOG_AT("WEBAPI", err, "compose failed: user_id={} reason={}", user.user_id, composed.error);
            complete(error_response(composed.error_status, composed.error.c_str())); return;
        }
        auto message_id = composed.message_id;
        SubmitRequest submission{user.address, std::move(password),
                                  std::move(composed.recipients), std::move(composed.raw)};
        submission.user_id = user.user_id;
        submission.subject = message.subject;
        submission.message_id = message_id;
        submitter.submit_async(std::move(submission),
            [&sessions, token, message_id, complete](SubmitResult result) {
                if (result.status == SubmitStatus::Accepted) {
                    // 默认 direct 后端已确认持久化；可注入适配器决定接受边界。
                    complete(json_response(202, {{"status", "accepted"}, {"message_id", message_id}})); return;
                }
                if (result.status == SubmitStatus::AuthFailed) {
                    sessions.erase_async(token, [complete](bool ok) {
                        complete(error_response(ok ? 401 : 503, ok ? "please log in again" : "session store unavailable",
                                                ok ? SessionStore::clear_cookie_header() : ""));
                    }); return;
                }
                const bool unknown = result.status == SubmitStatus::OutcomeUnknown;
                const bool unavailable = result.status == SubmitStatus::Unavailable;
                const int status = unknown ? 504 : unavailable ? 503 :
                                   result.status == SubmitStatus::Rejected ? 422 : 502;
                complete(json_response(status, {
                    {"error", unknown ? "submission outcome unknown; verify delivery before retrying" :
                              unavailable ? "submission service unavailable" : "submission rejected"},
                    {"status", unknown ? "unknown" : "failed"}, {"message_id", message_id},
                    {"retryable", unavailable}, {"smtp_code", result.smtp_code}, {"stage", result.stage}}));
            });
    });
}

} // namespace mail_system::webmail::api
