#ifndef MAIL_SYSTEM_OUTBOX_REPOSITORY_H
#define MAIL_SYSTEM_OUTBOX_REPOSITORY_H

#include "mail_system/back/db/db_pool.h"
#include "mail_system/back/entities/mail.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mail_system {
namespace outbound {

enum class OutboxStatus : int {
    PENDING = 0,
    SENDING = 1,
    SENT = 2,
    RETRY = 3,
    DEAD = 4,
};

struct OutboxRecord {
    std::uint64_t id{0};
    std::uint64_t mail_id{0};
    std::string sender;
    std::string recipient;
    int attempt_count{0};
    int max_attempts{8};
};

class OutboxRepository {
public:
    explicit OutboxRepository(std::shared_ptr<DBPool> db_pool);

    bool enqueue_from_mail(const mail& mail_data,
                           const std::string& local_domain,
                           std::vector<std::uint64_t>* outbox_ids = nullptr);

    std::vector<OutboxRecord> claim_batch(const std::string& worker_id,
                                          std::size_t limit,
                                          int lease_seconds);

    bool mark_sent(std::uint64_t outbox_id, const std::string& smtp_response);
    bool mark_retry(std::uint64_t outbox_id,
                    const std::string& error_message,
                    int retry_delay_seconds);
    bool mark_dead(std::uint64_t outbox_id, const std::string& error_message);
    bool requeue_expired_leases();

private:
    std::shared_ptr<DBPool> db_pool_;

    static std::string extract_domain(const std::string& email);
    static std::string escape_or_empty(const std::shared_ptr<IDBConnection>& conn,
                                       const std::string& value);
};

} // namespace outbound
} // namespace mail_system

#endif // MAIL_SYSTEM_OUTBOX_REPOSITORY_H
