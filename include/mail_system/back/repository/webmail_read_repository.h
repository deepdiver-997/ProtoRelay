#pragma once
#include "mail_system/back/entities/webmail_read.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace mail_system::router { class IShardRouter; }
namespace mail_system::repository {
// Callbacks preserve the driver's inline/deferred completion behavior. The SQL
// implementation retains one connection lease throughout each serial query chain.
// Unavailable pools/connections set server_error; failed primary queries leave
// ok false. Auxiliary sender/recipient/attachment failures retain legacy tolerance.
// The caller supplies normalized pagination: page >= 1, 1 <= page_size <= 100.
class IWebmailReadRepository {
public:
    virtual ~IWebmailReadRepository() = default;
    virtual void list_mailboxes_async(uint64_t user_id, const std::string& address,
        std::function<void(webmail::MailboxListResult)> complete) = 0;
    virtual void list_messages_async(uint64_t user_id, const std::string& address,
        uint64_t mailbox_id, uint32_t page, uint32_t page_size,
        std::function<void(webmail::MessagePageResult)> complete) = 0;
    virtual void get_message_metadata_async(uint64_t user_id, const std::string& address,
        uint64_t mailbox_id, uint64_t mail_id,
        std::function<void(webmail::MessageMetadataResult)> complete) = 0;
};
std::shared_ptr<IWebmailReadRepository> make_sql_webmail_read_repository(
    std::shared_ptr<router::IShardRouter> router);
} // namespace mail_system::repository
