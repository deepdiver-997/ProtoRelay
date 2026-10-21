#pragma once
#include "mail_system/back/entities/account.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace mail_system::router { class IShardRouter; }
namespace mail_system::repository {
// A login keeps one account connection through read -> optional legacy upgrade
// -> fresh read -> last-login. The context owns both its pool and its lease;
// callers never handle database objects or result columns.
class IAccountCredentialAccess {
public:
    using ReadComplete = std::function<void(CredentialLookup)>;
    using Complete = std::function<void(bool)>;
    virtual ~IAccountCredentialAccess() = default;
    virtual int shard() const = 0;
    virtual void read_async(ReadComplete) = 0;
    // A successful execution includes a CAS miss (zero affected rows), matching
    // the existing cache policy. Fresh login must still re-read credentials.
    virtual void upgrade_legacy_hash_async(const std::string& expected,
                                           const std::string& replacement, Complete) = 0;
    virtual void touch_login_async(Complete) = 0;
};
class IAccountRepository {
public:
    virtual ~IAccountRepository() = default;
    // Preserve raw route failure: authentication/policy fall back to shard zero,
    // while the trusted submission boundary rejects an unresolved actor.
    virtual int route_address(const std::string& address) = 0;
    virtual std::shared_ptr<IAccountCredentialAccess> open_credentials(
        const std::string& address, int shard) = 0;
    virtual void recipient_async(const std::string& address,
                                  std::function<void(RecipientLookup)>) = 0;
    virtual void aliases_async(const std::string& address,
                                std::function<void(AliasLookup)>) = 0;
    // The conditional update and ROW_COUNT are one indivisible lease operation.
    virtual void consume_quota_async(const std::string& address, int daily_limit,
                                      std::function<void(QuotaOutcome)>) = 0;
    // Fresh, synchronous Worker operation used by trusted in-process submission.
    // It releases the account lease before policy/storage/queue work starts.
    virtual ActorStatus verify_actor(const std::string& address, uint64_t user_id, int shard) = 0;
};
std::shared_ptr<IAccountRepository> make_account_repository(std::shared_ptr<router::IShardRouter>);
} // namespace mail_system::repository
