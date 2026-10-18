#pragma once
#include "mail_system/back/smtp_client/secret.h"

namespace mail_system::webmail {
using SessionPassword = smtp::Secret;
using SessionPasswordPtr = smtp::SecretPtr;
} // namespace mail_system::webmail
