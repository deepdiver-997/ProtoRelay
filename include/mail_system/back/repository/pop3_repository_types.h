#pragma once
#include "framework/db/db_pool.h"
#include <cstdint>
#include <memory>
#include <set>
#include <utility>

namespace mail_system::repository {
// A ScopedConnection borrows its pool. Declare the owner first so the lease is
// returned before the last pool reference is destroyed, including delayed work.
struct Pop3DbSession {
    std::shared_ptr<pr::DBPool> pool;
    std::shared_ptr<pr::ScopedConnection> lease;
    bool valid() const { return lease && lease->is_valid(); }
};
using Pop3DbContext = std::shared_ptr<Pop3DbSession>;
inline Pop3DbContext make_pop3_db_context(std::shared_ptr<pr::DBPool> pool) {
    auto value = std::make_shared<Pop3DbSession>();
    value->pool = std::move(pool);
    if (value->pool) value->lease = value->pool->acquire_connection();
    return value;
}
// Legacy helper compatibility only: the caller must keep the owning pool alive
// until completion. New protocol paths use make_pop3_db_context instead.
inline Pop3DbContext borrow_pop3_db_context(std::shared_ptr<pr::ScopedConnection> lease) {
    auto value = std::make_shared<Pop3DbSession>();
    value->lease = std::move(lease);
    return value;
}
struct Pop3DeletionRequest {
    uint64_t user_id = 0, mailbox_id = 0;
    std::set<uint64_t> mail_ids;
};
struct Pop3DeletionResult {
    bool ok = false;
    bool may_have_changed = false;
};
} // namespace mail_system::repository
