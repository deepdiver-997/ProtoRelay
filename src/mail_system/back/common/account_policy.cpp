#include "mail_system/back/common/account_policy.h"
#include "mail_system/back/common/logger.h"
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mail_system {
void AccountPolicy::login_async(const std::string& address, const std::string& password,
                               std::function<void(AccountAuthResult)> complete, bool fresh) {
    authenticator_->login_async(address, password, std::move(complete), fresh);
}
void AccountPolicy::user_exists_async(const std::string& address, std::function<void(bool)> cb) {
    AuthCacheEntry cached;
    if (recipients_->lookup(address, cached)) { cb(cached.status == 1); return; }
    auto accounts = accounts_;
    accounts->recipient_async(address, [accounts, recipients = recipients_, address, cb = std::move(cb)]
                                       (repository::RecipientLookup lookup) mutable {
        using Status = repository::AccountLookupStatus;
        if (lookup.status == Status::unavailable) { cb(false); return; }
        // Preserve the historical recipient negative cache on query failure,
        // while unavailable connections do not create a negative cache entry.
        const int status = lookup.status == Status::found ? static_cast<int>(lookup.account_status) : 0;
        if (lookup.status == Status::query_failed)
            LOG_AT("ACCOUNT", warn, "recipient query failed address={}", address);
        recipients->store(address, {"", status, 0, 0});
        cb(status == 1);
    });
}
void AccountPolicy::alias_targets_async(const std::string& address,
                                        std::function<void(std::vector<std::string>)> cb) {
    std::vector<std::string> cached;
    bool stale = false;
    if (aliases_->get(address, cached, stale) && !stale) { cb(std::move(cached)); return; }
    auto accounts = accounts_;
    accounts->aliases_async(address, [accounts, aliases = aliases_, address, cb = std::move(cb)]
                                     (repository::AliasLookup lookup) mutable {
        if (lookup.success) aliases->put(address, lookup.targets);
        else LOG_AT("ACCOUNT", warn, "alias query failed address={}", address);
        cb(std::move(lookup.targets));
    });
}
void AccountPolicy::check_send_quota_async(const std::string& address, int limit,
                                           std::function<void(bool)> cb) {
    if (!cb) return;
    if (limit <= 0) { cb(true); return; }
    auto accounts = accounts_;
    accounts->consume_quota_async(address, limit, [accounts, cb = std::move(cb)](repository::QuotaOutcome outcome) {
        using Outcome = repository::QuotaOutcome;
        if (outcome == Outcome::unavailable || outcome == Outcome::update_failed)
            LOG_SMTP_WARN("quota check unavailable or update failed (fail-open)");
        if (outcome == Outcome::count_failed)
            LOG_AT("ACCOUNT", warn, "quota affected rows unavailable or invalid");
        cb(outcome == Outcome::consumed || outcome == Outcome::unavailable || outcome == Outcome::update_failed);
    });
}
bool AccountPolicy::recipient_allowed(const std::string& address, const std::string& domain, bool external) {
    auto at = address.rfind('@');
    return external || (at != std::string::npos &&
        algorithm::to_lower(algorithm::trim(address.substr(at + 1))) == algorithm::to_lower(domain));
}
} // namespace mail_system
