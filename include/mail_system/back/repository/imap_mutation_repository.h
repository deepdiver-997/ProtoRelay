#pragma once

#include "framework/db/db_pool.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mail_system::repository {

struct ImapMailboxScope {
    uint64_t user_id = 0;
    uint64_t mailbox_id = 0;
};

// IDs have already been selected from the user's mailbox by the protocol's
// sequence/UID adapter. A missing flag leaves that attribute unchanged.
struct ImapStoreRequest {
    ImapMailboxScope scope;
    std::vector<uint64_t> mail_ids;
    std::string recipient_fallback;
    std::optional<bool> seen;
    std::optional<bool> flagged;
    std::optional<bool> deleted;
};
struct ImapMutationResult {
    bool ok = false;
};
struct ImapCopyMoveRequest {
    ImapMailboxScope source;
    uint64_t target_mailbox_id = 0;
    std::vector<uint64_t> mail_ids;
    bool move = false;
};
struct ImapCopyMoveResult {
    bool copy_ok = false;
    std::size_t copied = 0;
    bool source_marked = false;
};

// SQL, recipient lookup, affected-row mapping and serial mutation chains live
// behind this boundary. Each operation keeps its connection lease until its
// final callback returns. The caller must keep the lease's owning DBPool alive
// throughout the operation (ScopedConnection does not own that pool).
// Completion preserves the driver's inline/deferred behavior; protocol state
// must resume on its session executor.
class IImapMutationRepository {
public:
    using Lease = std::shared_ptr<pr::ScopedConnection>;
    using Complete = std::function<void(ImapMutationResult)>;
    using CopyComplete = std::function<void(ImapCopyMoveResult)>;
    virtual ~IImapMutationRepository() = default;
    virtual void store_async(Lease, ImapStoreRequest, Complete) = 0;
    virtual void expunge_async(Lease, ImapMailboxScope, Complete) = 0;
    virtual void copy_move_async(Lease, ImapCopyMoveRequest, CopyComplete) = 0;
};
std::shared_ptr<IImapMutationRepository> make_sql_imap_mutation_repository();

} // namespace mail_system::repository
