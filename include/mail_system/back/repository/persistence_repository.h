#pragma once
#include "mail_system/back/entities/mail.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mail_system::router { class IShardRouter; }
namespace mail_system::repository {
struct PersistenceOptions {
    std::string local_domain = "example.com";
    // Controls PENDING rows. The legacy reserved insertion keeps its fixed 8.
    size_t max_attempts = 8;
    std::string reserve_owner;
    int reserve_lease_seconds = 0;
};
struct PersistenceRequest {
    std::shared_ptr<const ::mail> message;
    PersistenceOptions options;
};
enum class PersistenceDisposition { failed, committed, file_only, deduplicated };
enum class PersistenceTransaction { not_started, committed, rolled_back, rollback_failed };
// The persistence boundary returns records, not mutable output pointers or an
// OutboundServer dependency. Records are publishable only after commit.
struct PersistedOutboxRecord {
    uint64_t id = 0, mail_id = 0;
    std::string sender, recipient, body_path;
    int attempt_count = 0, max_attempts = 8;
};
struct PersistenceResult {
    PersistenceDisposition disposition = PersistenceDisposition::failed;
    PersistenceTransaction transaction = PersistenceTransaction::not_started;
    int source_shard = 0;
    std::string error;
    std::vector<PersistedOutboxRecord> outbox_records;
    bool success() const { return disposition != PersistenceDisposition::failed; }
};
struct RecipientPersistenceResult {
    std::string recipient;
    int shard = 0;
    bool ok = false;
    PersistenceTransaction transaction = PersistenceTransaction::not_started;
    std::string error;
};
struct PersistenceEraseRequest {
    int shard = 0;
    std::vector<uint64_t> mail_ids;
    bool delete_attachment_metadata = false;
};
struct PersistenceEraseResult { bool ok = true; std::string error; };
class IPersistenceRepository {
public:
    using Complete = std::function<void(PersistenceResult)>;
    virtual ~IPersistenceRepository() = default;
    // Owns mail/options/pool/lease until completion, preserving inline/deferred
    // driver callbacks. Never holds a queue or schedules storage/file cleanup.
    virtual void persist_async(PersistenceRequest, Complete) = 0;
    // Independent transactions on the caller's Worker, after primary commit.
    // Failures cannot rewrite a previously confirmed primary result.
    virtual std::vector<RecipientPersistenceResult> persist_recipient_copies(const PersistenceRequest&, int source_shard) = 0;
    // Best effort metadata-only cleanup; the queue owns body/attachment files.
    virtual PersistenceEraseResult erase_metadata(const PersistenceEraseRequest&) = 0;
};
std::shared_ptr<IPersistenceRepository> make_sql_persistence_repository(std::shared_ptr<router::IShardRouter>);
} // namespace mail_system::repository
