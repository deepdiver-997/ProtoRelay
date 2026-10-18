#pragma once

#include "mail_system/back/webmail/api/auth_api.h"
#include "mail_system/back/webmail/smtp_submitter.h"

namespace mail_system::webmail::api {

// POST /api/send。Content-Type 必须为 application/json（阻止跨源简单表单提交）。
void send_message(const pr::http::HttpRequest& req, SessionStore& sessions,
                  ISmtpSubmitter& submitter, const Complete& complete);

} // namespace mail_system::webmail::api
