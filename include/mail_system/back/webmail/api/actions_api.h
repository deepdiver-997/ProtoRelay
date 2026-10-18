#pragma once
#include "mail_system/back/webmail/api/auth_api.h"
#include "mail_system/back/webmail/mail_actions.h"
namespace mail_system::webmail::api {
bool route_actions(const pr::http::HttpRequest& req, SessionStore& sessions, MailActions* actions, const Complete& complete);
}
