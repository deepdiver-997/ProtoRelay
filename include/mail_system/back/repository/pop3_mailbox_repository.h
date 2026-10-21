#pragma once
#include "mail_system/back/repository/pop3_repository_types.h"
#include "mail_system/back/entities/mailbox.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace mail_system::repository {
// SQL and column mapping stay here. Callbacks preserve inline/deferred shape;
// the context keeps both the lease and its owning pool alive through completion.
class IPop3MailboxRepository {
public:
    using InboxComplete = std::function<void(bool, uint64_t)>;
    using MessagesComplete = std::function<void(bool, std::vector<MailboxMailInfo>)>;
    using DeleteComplete = std::function<void(Pop3DeletionResult)>;
    virtual ~IPop3MailboxRepository() = default;
    virtual void get_inbox_async(Pop3DbContext, uint64_t user_id, InboxComplete) = 0;
    // Return identity/body fields in shared mailbox order, including is_deleted entries.
    virtual void get_messages_async(Pop3DbContext, uint64_t user_id, uint64_t mailbox_id, MessagesComplete) = 0;
    virtual void apply_deletions_async(Pop3DbContext, Pop3DeletionRequest, DeleteComplete) = 0;
};
std::shared_ptr<IPop3MailboxRepository> make_sql_pop3_mailbox_repository();
} // namespace mail_system::repository
