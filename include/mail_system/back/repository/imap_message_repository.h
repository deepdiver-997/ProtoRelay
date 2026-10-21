#pragma once
#include "framework/db/db_pool.h"
#include "mail_system/back/entities/mailbox.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mail_system::repository {
struct ImapMailRecord {
    uint64_t mail_id = 0;
    std::string subject;
    std::string body_key;
    int64_t timestamp = 0;
};
struct ImapMailLink {
    uint64_t mail_id = 0, user_id = 0, mailbox_id = 0;
    std::string sender, recipient;
    int status = 0;
};
// Keeps the lease through completion; its owning pool must outlive all calls.
// No protocol or storage IO belongs to this metadata/SQL boundary.
class IImapMessageRepository {
public:
    using Lease = std::shared_ptr<pr::ScopedConnection>;
    using Complete = std::function<void(bool)>;
    using MailComplete = std::function<void(bool, MailboxMailInfo)>;
    using AddressComplete = std::function<void(std::string)>;
    using RecipientsComplete = std::function<void(std::vector<std::string>)>;
    virtual ~IImapMessageRepository() = default;
    virtual void get_mail_async(Lease, uint64_t mail_id, MailComplete) = 0;
    virtual void get_sender_async(Lease, uint64_t mail_id, AddressComplete) = 0;
    virtual void get_recipients_async(Lease, uint64_t mail_id, RecipientsComplete) = 0;
    virtual void get_user_address_async(Lease, uint64_t user_id, AddressComplete) = 0;
    virtual void insert_mail_async(Lease, ImapMailRecord, Complete) = 0;
    virtual void link_mail_async(Lease, ImapMailLink, Complete) = 0;
    // Best effort across all three tables, even if an earlier deletion fails.
    virtual void remove_mail_async(Lease, uint64_t mail_id, Complete) = 0;
};
std::shared_ptr<IImapMessageRepository> make_sql_imap_message_repository();
} // namespace mail_system::repository
