#ifndef MAIL_SYSTEM_OUTBOUND_UTILS_H
#define MAIL_SYSTEM_OUTBOUND_UTILS_H

#include "mail_system/back/mailServer/outbound/outbox_repository.h"
#include "mail_system/back/mailServer/outbound/outbound_config.h"
#include "mail_system/back/entities/mail.h"

#include <string>

namespace mail_system {
namespace outbound {

bool ensure_mail_raw_payload_loaded(mail& mail_data);

// 对完整 RFC5322 报文按 identity_config 做 DKIM 签名：成功在头部前插入
// DKIM-Signature 并返回新报文；未启用/失败原样返回（原因写入 *error_out，可为 nullptr）。
std::string sign_payload_if_dkim(const std::string& raw_payload,
                                 const OutboundConfig& identity_config,
                                 std::string* error_out = nullptr);

// Build RFC5322 wire message and optionally include DKIM-Signature.
std::string build_outbound_message(const OutboxRecord& record,
                                   const mail* hot_mail,
                                   const std::string& header_from,
                                   const OutboundConfig& identity_config,
                                   bool* dkim_applied,
                                   std::string* dkim_error,
                                   std::string* message_id_out = nullptr);

} // namespace outbound
} // namespace mail_system

#endif // MAIL_SYSTEM_OUTBOUND_UTILS_H
