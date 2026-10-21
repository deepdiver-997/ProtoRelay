#pragma once
#include "mail_system/back/entities/webmail_persistence.h"
#include <cstdint>
#include <memory>

namespace mail_system::router { class IShardRouter; }
namespace mail_system::repository {
// Synchronous persistence boundary. The service schedules this work on Worker;
// no network executor or asynchronous callback semantics are implied here.
class IWebmailActionRepository {
public:
    virtual ~IWebmailActionRepository() = default;
    virtual webmail::MailActionResult update_flags(const webmail::MailFlagUpdate&) = 0;
    // Authorization is complete and its connection returned before this method
    // returns. The caller opens a bounded provider download using the opaque key.
    virtual webmail::AuthorizedAttachment find_attachment(
        const webmail::MailResourceScope&, uint64_t attachment_id) = 0;
};
std::shared_ptr<IWebmailActionRepository> make_sql_webmail_action_repository(
    std::shared_ptr<router::IShardRouter> router);
} // namespace mail_system::repository
