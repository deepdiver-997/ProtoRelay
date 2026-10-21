#pragma once
#include "framework/db/db_pool.h"
#include "mail_system/back/entities/mailbox.h"
#include <functional>
#include <vector>

namespace mail_system::repository {
// Read-side application boundary. The caller retains its generic connection
// lease across existing protocol/write chains; SQL and result columns belong
// only to the implementation. Callbacks preserve the underlying connection's
// inline/deferred behavior, and the lease stays alive until completion.
// Not-found/error defaults match the established IMAP behavior (0, or UIDNEXT 1).
class IMailboxRepository {
public:
    using Lease = std::shared_ptr<pr::ScopedConnection>;
    using ListCallback = std::function<void(bool, std::vector<MailboxSummary>)>;
    using MessagesCallback = std::function<void(bool, std::vector<MailboxMailInfo>)>;
    virtual ~IMailboxRepository() = default;
    virtual void get_mailboxes_async(Lease, uint64_t user_id, ListCallback) = 0;
    // The name is decoded UTF-8; IMAP modified UTF-7 is handled by the adapter.
    virtual void find_mailbox_id_async(Lease, uint64_t user_id, const std::string& name,
                                       std::function<void(uint64_t)>) = 0;
    virtual void get_mailbox_mails_async(Lease, uint64_t mailbox_id, uint64_t user_id, MessagesCallback) = 0;
    virtual void get_mailbox_count_async(Lease, uint64_t mailbox_id, uint64_t user_id,
                                         std::function<void(size_t)>) = 0;
    virtual void get_mailbox_unseen_count_async(Lease, uint64_t mailbox_id, uint64_t user_id,
                                                std::function<void(size_t)>) = 0;
    virtual void get_mailbox_uidnext_async(Lease, uint64_t mailbox_id, uint64_t user_id,
                                           std::function<void(uint64_t)>) = 0;
};
std::shared_ptr<IMailboxRepository> make_sql_mailbox_repository();
} // namespace mail_system::repository
