#include "mail_system/back/common/account_authentication.h"
#include "mail_system/back/common/credential_stamp.h"
#include "mail_system/back/common/password_rehash.h"
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace mail_system {
void AccountAuthenticationService::login_async(const std::string& address, const std::string& password,
                                               Complete cb, bool fresh) {
    if (!cb) return;
    auto accounts = accounts_;
    auto cache = cache_;
    auto workers = workers_;
    auto task = [address, password, accounts, cache, fresh, workers, cb = std::move(cb)]() mutable {
        AccountAuthResult out;
        out.address = address;
        const int route = accounts->route_address(address);
        out.shard = route < 0 ? 0 : route;

        AuthCacheEntry cached;
        if (!fresh && cache && cache->lookup(address, cached)) {
            if (cached.status == 1 && verify_password_hash(password, cached.password_hash)) {
                out.ok = true;
                out.user_id = cached.user_id;
                out.shard = cached.shard;
                out.credential_stamp = credential_stamp(cached.password_hash);
            }
            cb(std::move(out));
            return;
        }
        auto access = accounts->open_credentials(address, out.shard);
        if (!access) {
            out.server_error = true;
            cb(std::move(out));
            return;
        }
        access->read_async([access, address, password, cache, fresh, workers, out,
                            cb = std::move(cb)](repository::CredentialLookup lookup) mutable {
            auto verify = [access, address, password, cache, fresh, workers, out,
                           lookup = std::move(lookup), cb = std::move(cb)]() mutable {
                using Status = repository::AccountLookupStatus;
                if (lookup.status != Status::found) {
                    out.server_error = lookup.status == Status::query_failed || lookup.status == Status::unavailable;
                    cb(std::move(out));
                    return;
                }
                // Preserve the existing initial-read and cache status mapping.
                const int status = static_cast<int>(lookup.credentials.status);
                if (status != 1) { cb(std::move(out)); return; }
                auto stored = std::move(lookup.credentials.stored_hash);
                AuthCacheEntry entry{stored, status, lookup.credentials.user_id, out.shard};
                if (cache) cache->store(address, entry);
                if (!verify_password_hash(password, stored)) { cb(std::move(out)); return; }
                out.ok = true;
                out.user_id = entry.user_id;
                out.credential_stamp = credential_stamp(stored);
                rehash_password_if_needed_async(access, cache, address, password, stored, entry,
                    [access, out, password, fresh, workers, cb = std::move(cb)]() mutable {
                        auto finish = [access, cb = std::move(cb)](AccountAuthResult current) mutable {
                            if (!current.ok) { cb(std::move(current)); return; }
                            access->touch_login_async([access, current = std::move(current),
                                                       cb = std::move(cb)](bool ok) mutable {
                                if (!ok) LOG_AT("ACCOUNT", warn, "last_login update failed user_id={}", current.user_id);
                                cb(std::move(current));
                            });
                        };
                        if (!fresh) { finish(std::move(out)); return; }
                        // A legacy CAS can lose to a password reset. Only the
                        // authoritative second read may mint a fresh web session.
                        access->read_async([access, out, password, workers, finish = std::move(finish)]
                                           (repository::CredentialLookup live) mutable {
                            auto verify = [access, out, password, live = std::move(live),
                                           finish = std::move(finish)]() mutable {
                                using Status = repository::AccountLookupStatus;
                                if (live.status != Status::found) {
                                    out.ok = false;
                                    out.server_error = live.status == Status::query_failed || live.status == Status::unavailable;
                                    finish(std::move(out));
                                    return;
                                }
                                if (live.credentials.status != 1 || live.credentials.user_id != out.user_id) {
                                    out.ok = false;
                                    finish(std::move(out));
                                    return;
                                }
                                auto stamp = credential_stamp(live.credentials.stored_hash);
                                if (stamp != out.credential_stamp &&
                                    !verify_password_hash(password, live.credentials.stored_hash)) {
                                    out.ok = false;
                                    finish(std::move(out));
                                    return;
                                }
                                out.credential_stamp = std::move(stamp);
                                finish(std::move(out));
                            };
                            if (workers) workers->post(std::move(verify)); else verify();
                        });
                    });
            };
            // Driver completions can arrive on an IO executor. All password
            // verification and bcrypt work must return to the injected Worker.
            if (workers) workers->post(std::move(verify)); else verify();
        });
    };
    if (workers) workers->post(std::move(task)); else task();
}
} // namespace mail_system
