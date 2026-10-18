#include "mail_system/back/webmail/session_store.h"
#include "mail_system/back/common/credential_stamp.h"
#include "mail_system/back/common/safe_parse.h"
#include "mail_system/back/common/logger.h"
#include "mail_system/back/db/sql_queries.h"
#include <algorithm>

namespace mail_system::webmail {
namespace {
bool valid_token(const std::string &token) {
    return token.size() == 64 && std::all_of(token.begin(), token.end(), [](unsigned char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
void run(const std::shared_ptr<pr::ThreadPoolBase> &workers, std::function<void()> task) {
    if (workers)
        workers->post(std::move(task));
    else
        task();
}
} // namespace

void SessionStore::put_async(std::string token, WebSessionInfo user, SessionPasswordPtr password,
                             std::function<void(bool)> cb) {
    if (!database_) {
        put(token, user.user_id, user.address, std::move(password));
        cb(true);
        return;
    }
    auto router = router_;
    auto ttl = ttl_.count();
    run(workers_, [router, ttl, token = std::move(token), user = std::move(user), cb = std::move(cb)] {
        auto pool = router ? router->get_db_pool(0) : nullptr;
        auto conn = pool ? pool->acquire_connection() : ScopedConnection::invalid();
        if (!valid_token(token) || user.credential_stamp.empty() || !conn->is_valid()) {
            LOG_AT("WEBAPI", err, "session insert unavailable user_id={}", user.user_id);
            cb(false);
            return;
        }
        bool ok =
            (*conn)->execute("INSERT INTO web_sessions "
                             "(token_hash,user_id,account_address,account_shard,credential_stamp,created_at,expires_at) "
                             "VALUES (?,?,?,?,?,UTC_TIMESTAMP(),TIMESTAMPADD(SECOND,?,UTC_TIMESTAMP()))",
                             {credential_stamp(token), std::to_string(user.user_id), user.address,
                              std::to_string(user.shard), user.credential_stamp, std::to_string(ttl)});
        if (!ok)
            LOG_AT("WEBAPI", err, "session insert failed user_id={}", user.user_id);
        if (ok && !(*conn)->execute("DELETE FROM web_sessions WHERE expires_at<=UTC_TIMESTAMP() LIMIT 128"))
            LOG_AT("WEBAPI", warn, "expired session cleanup failed");
        cb(ok);
    });
}

void SessionStore::lookup_async(std::string token, std::function<void(LookupResult)> cb) const {
    if (!database_) {
        LookupResult r;
        r.found = lookup(token, r.user, &r.password);
        cb(std::move(r));
        return;
    }
    if (!valid_token(token)) {
        cb({});
        return;
    }
    auto router = router_;
    run(workers_, [router, token = std::move(token), cb = std::move(cb)] {
        LookupResult r;
        auto fail = [&] {
            r.server_error = true;
            LOG_AT("WEBAPI", err, "session lookup DB unavailable");
            cb(r);
        };
        auto pool = router ? router->get_db_pool(0) : nullptr;
        auto conn = pool ? pool->acquire_connection() : ScopedConnection::invalid();
        if (!conn->is_valid()) {
            fail();
            return;
        }
        auto rows =
            (*conn)->query("SELECT user_id,account_address,account_shard,credential_stamp FROM web_sessions "
                           "WHERE token_hash=? AND expires_at>UTC_TIMESTAMP()",
                           {credential_stamp(token)});
        // Release registry lease before borrowing an account connection (same shard may have a one-slot
        // pool).
        conn.reset();
        if (!rows) {
            fail();
            return;
        }
        if (!rows->get_row_count()) {
            cb(r);
            return;
        }
        r.user.user_id = safe_stoull(rows->get_value(0, "user_id"));
        r.user.address = rows->get_value(0, "account_address");
        auto shard = safe_stoull(rows->get_value(0, "account_shard"));
        if (!router || shard >= router->shard_count()) {
            fail();
            return;
        }
        r.user.shard = static_cast<int>(shard);
        r.user.credential_stamp = rows->get_value(0, "credential_stamp");
        auto account_pool = router->get_db_pool(shard);
        auto account = account_pool ? account_pool->acquire_connection() : ScopedConnection::invalid();
        if (!account->is_valid()) {
            fail();
            return;
        }
        auto user = (*account)->query(db::sql::build_auth_user_query(), {r.user.address});
        if (!user) {
            fail();
            return;
        }
        r.found = user->get_row_count() && safe_stoull(user->get_value(0, "id")) == r.user.user_id &&
                  safe_stoull(user->get_value(0, "status")) == 1 &&
                  credential_stamp(user->get_value(0, "password")) == r.user.credential_stamp;
        cb(r);
    });
}

void SessionStore::erase_async(std::string token, std::function<void(bool)> cb) {
    if (!database_) {
        erase(token);
        cb(true);
        return;
    }
    if (!valid_token(token)) {
        cb(true);
        return;
    }
    auto router = router_;
    run(workers_, [router, token = std::move(token), cb = std::move(cb)] {
        auto pool = router ? router->get_db_pool(0) : nullptr;
        auto conn = pool ? pool->acquire_connection() : ScopedConnection::invalid();
        bool ok = conn->is_valid() &&
                  (*conn)->execute("DELETE FROM web_sessions WHERE token_hash=?", {credential_stamp(token)});
        if (!ok)
            LOG_AT("WEBAPI", err, "session revoke failed");
        cb(ok);
    });
}
} // namespace mail_system::webmail
