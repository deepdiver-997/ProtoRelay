#ifndef MAIL_SYSTEM_OUTBOUND_UTILS_H
#define MAIL_SYSTEM_OUTBOUND_UTILS_H

#include "mail_system/back/outbound/smtp_outbound_client.h"

#include <string>

namespace mail_system {
namespace outbound {

// Build RFC5322 wire message and optionally include DKIM-Signature.
std::string build_outbound_message(const OutboxRecord& record,
                                   const std::string& header_from,
                                   const OutboundIdentityConfig& identity_config,
                                   bool* dkim_applied,
                                   std::string* dkim_error);

} // namespace outbound
} // namespace mail_system

#endif // MAIL_SYSTEM_OUTBOUND_UTILS_H
