#pragma once
#include "framework/db/db_pool.h"
#include "mail_system/back/repository/imap_mutation_repository.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace mail_system::repository {
enum class ImapFolderStatus { ok, not_found, protected_mailbox, database_error };
struct ImapFolderResult {
    ImapFolderStatus status = ImapFolderStatus::database_error;
    bool may_have_changed = false;
};
// UTF-7 conversion and tagged replies belong to the protocol adapter. The
// caller keeps the lease's pool alive; callbacks retain inline/deferred shape.
class IImapFolderRepository {
public:
    using Lease = std::shared_ptr<pr::ScopedConnection>;
    using Complete = std::function<void(ImapFolderResult)>;
    virtual ~IImapFolderRepository() = default;
    virtual void create_async(Lease, uint64_t user_id, std::string decoded_name, Complete) = 0;
    virtual void remove_async(Lease, ImapMailboxScope, Complete) = 0;
    virtual void rename_async(Lease, ImapMailboxScope, std::string decoded_name, Complete) = 0;
};
std::shared_ptr<IImapFolderRepository> make_sql_imap_folder_repository();
} // namespace mail_system::repository
