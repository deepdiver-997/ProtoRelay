#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mail_system::repository {
// No cache, password verification or transport policy belongs to these results.
enum class AccountLookupStatus { found, not_found, unavailable, query_failed };
struct AccountCredentials {
    uint64_t user_id = 0;
    uint64_t status = 0;
    std::string stored_hash;
};
struct CredentialLookup {
    AccountLookupStatus status = AccountLookupStatus::query_failed;
    AccountCredentials credentials;
};
struct RecipientLookup {
    AccountLookupStatus status = AccountLookupStatus::query_failed;
    uint64_t account_status = 0;
};
struct AliasLookup {
    bool success = false;
    std::vector<std::string> targets;
};
enum class QuotaOutcome { consumed, exceeded, unavailable, update_failed, count_failed };
enum class ActorStatus { valid, denied, unavailable };
} // namespace mail_system::repository
