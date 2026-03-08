#ifndef MAIL_SYSTEM_MX_ROUTING_UTILS_H
#define MAIL_SYSTEM_MX_ROUTING_UTILS_H

#include "mail_system/back/entities/mail.h"
#include "mail_system/back/outbound/dns_resolver.h"
#include "mail_system/back/outbound/outbox_repository.h"

#include <string>
#include <vector>

namespace mail_system {
namespace outbound {

bool has_external_recipient(const mail& mail_data, const std::string& local_domain);

std::vector<std::string> build_target_hosts(const OutboxRecord& record,
                                            IDnsResolver* resolver);

} // namespace outbound
} // namespace mail_system

#endif // MAIL_SYSTEM_MX_ROUTING_UTILS_H
