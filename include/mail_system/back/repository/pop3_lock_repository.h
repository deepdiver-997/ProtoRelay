#pragma once
#include "mail_system/back/repository/pop3_repository_types.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace mail_system::repository {
enum class Pop3LockStatus { owned, not_owned, database_error };
struct Pop3LockKey { uint64_t user_id = 0; std::string session_id; };
// Timer/Worker/router policy stays in the adapter. Each DB chain is single-flight
// and retains its context through callback completion, including inline drivers.
class IPop3LockRepository {
public:
    using Complete = std::function<void(Pop3LockStatus)>;
    using ReleaseComplete = std::function<void(bool)>;
    virtual ~IPop3LockRepository() = default;
    virtual void acquire_async(Pop3DbContext, Pop3LockKey, Complete) = 0;
    virtual void verify_async(Pop3DbContext, Pop3LockKey, Complete) = 0;
    virtual void renew_async(Pop3DbContext, Pop3LockKey, Complete) = 0;
    virtual void release_async(Pop3DbContext, Pop3LockKey, ReleaseComplete) = 0;
    // Called only by the adapter's maintenance Worker, once per shard.
    virtual bool sweep_expired(Pop3DbContext) = 0;
};
std::shared_ptr<IPop3LockRepository> make_sql_pop3_lock_repository();
} // namespace mail_system::repository
