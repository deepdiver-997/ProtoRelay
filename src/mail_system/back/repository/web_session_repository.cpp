#include "mail_system/back/repository/web_session_repository.h"
#include "mail_system/back/common/credential_stamp.h"
#include "mail_system/back/common/safe_parse.h"
#include "framework/log/log_macros.h"
#include "mail_system/back/db/sql_queries.h"
#include "mail_system/back/router/i_shard_router.h"
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace mail_system::repository {
namespace {
// Refuse old multi-session schemas rather than silently violating single login.
bool single_login_schema(pr::IDBConnection* db) {
    auto indexes = db->query("SHOW INDEX FROM web_sessions WHERE Key_name='uk_web_sessions_account'");
    if (!indexes || indexes->get_row_count() != 2) return false;
    bool shard = false, user = false;
    for (size_t i = 0; i < 2; ++i) {
        if (indexes->get_value(i, "Non_unique") != "0") return false;
        const auto position = indexes->get_value(i, "Seq_in_index");
        const auto column = indexes->get_value(i, "Column_name");
        shard = shard || (position == "1" && column == "account_shard");
        user = user || (position == "2" && column == "user_id");
    }
    return shard && user;
}
class SqlWebSessionRepository final : public IWebSessionRepository {
public:
    explicit SqlWebSessionRepository(std::shared_ptr<router::IShardRouter> router)
        : router_(std::move(router)) {}

    bool put(const std::string& token, const webmail::WebSessionInfo& user, std::chrono::seconds ttl) override {
        if (!webmail::valid_web_session_token(token) || user.credential_stamp.empty()) return false;
        try {
            auto pool = router_ ? router_->get_db_pool(0) : nullptr;
            auto connection = pool ? pool->acquire_connection() : pr::ScopedConnection::invalid();
            if (!connection->is_valid()) return false;
            if (!single_login_schema(connection->operator->())) {
                LOG_AT("WEBAPI", err, "single-login session constraint missing or unavailable user_id={}", user.user_id);
                return false;
            }
            const bool ok = (*connection)->execute(
                "INSERT INTO web_sessions "
                "(token_hash,user_id,account_address,account_shard,credential_stamp,created_at,expires_at) "
                "VALUES (?,?,?,?,?,UTC_TIMESTAMP(),TIMESTAMPADD(SECOND,?,UTC_TIMESTAMP())) "
                "ON DUPLICATE KEY UPDATE "
                "token_hash=IF(account_shard=VALUES(account_shard) AND "
                "user_id=VALUES(user_id),VALUES(token_hash),NULL), "
                "account_address=VALUES(account_address),credential_stamp=VALUES(credential_stamp), "
                "created_at=VALUES(created_at),expires_at=VALUES(expires_at)",
                {credential_stamp(token), std::to_string(user.user_id), user.address, std::to_string(user.shard),
                 user.credential_stamp, std::to_string(ttl.count())});
            if (!ok) LOG_AT("WEBAPI", err, "session insert failed user_id={}", user.user_id);
            if (ok && !(*connection)->execute("DELETE FROM web_sessions WHERE expires_at<=UTC_TIMESTAMP() LIMIT 128"))
                LOG_AT("WEBAPI", warn, "expired session cleanup failed");
            return ok;
        } catch (const std::exception& error) {
            LOG_AT("WEBAPI", err, "session insert exception: {}", error.what());
            return false;
        }
    }

    webmail::WebSessionLookup lookup(const std::string& token) override {
        webmail::WebSessionLookup out;
        if (!webmail::valid_web_session_token(token)) return out;
        auto fail = [&] {
            out.server_error = true;
            LOG_AT("WEBAPI", err, "session lookup DB unavailable");
            return out;
        };
        try {
            auto pool = router_ ? router_->get_db_pool(0) : nullptr;
            auto connection = pool ? pool->acquire_connection() : pr::ScopedConnection::invalid();
            if (!connection->is_valid()) return fail();
            auto rows = (*connection)->query(
                "SELECT user_id,account_address,account_shard,credential_stamp FROM web_sessions "
                "WHERE token_hash=? AND expires_at>UTC_TIMESTAMP()", {credential_stamp(token)});
            if (!rows) return fail();
            if (!rows->get_row_count()) return out;
            out.user.user_id = safe_stoull(rows->get_value(0, "user_id"));
            out.user.address = rows->get_value(0, "account_address");
            const auto shard = safe_stoull(rows->get_value(0, "account_shard"));
            if (!router_ || shard >= router_->shard_count() || shard > static_cast<uint64_t>(std::numeric_limits<int>::max())) return fail();
            out.user.shard = static_cast<int>(shard);
            out.user.credential_stamp = rows->get_value(0, "credential_stamp");
            // Registry and account may use the same one-slot pool. Finish result
            // mapping, release the result and lease, then borrow for fresh auth.
            rows.reset();
            connection.reset();
            auto account_pool = router_->get_db_pool(static_cast<size_t>(shard));
            auto account = account_pool ? account_pool->acquire_connection() : pr::ScopedConnection::invalid();
            if (!account->is_valid()) return fail();
            auto user = (*account)->query(db::sql::build_auth_user_query(), {out.user.address});
            if (!user) return fail();
            out.found = user->get_row_count() && safe_stoull(user->get_value(0, "id")) == out.user.user_id &&
                safe_stoull(user->get_value(0, "status")) == 1 &&
                credential_stamp(user->get_value(0, "password")) == out.user.credential_stamp;
            return out;
        } catch (const std::exception& error) {
            LOG_AT("WEBAPI", err, "session lookup exception: {}", error.what());
            return fail();
        }
    }

    bool erase(const std::string& token) override {
        if (!webmail::valid_web_session_token(token)) return true;
        try {
            auto pool = router_ ? router_->get_db_pool(0) : nullptr;
            auto connection = pool ? pool->acquire_connection() : pr::ScopedConnection::invalid();
            const bool ok = connection->is_valid() && (*connection)->execute(
                "DELETE FROM web_sessions WHERE token_hash=?", {credential_stamp(token)});
            if (!ok) LOG_AT("WEBAPI", err, "session revoke failed");
            return ok;
        } catch (const std::exception& error) {
            LOG_AT("WEBAPI", err, "session revoke exception: {}", error.what());
            return false;
        }
    }
private:
    std::shared_ptr<router::IShardRouter> router_;
};
} // namespace
std::shared_ptr<IWebSessionRepository> make_sql_web_session_repository(
    std::shared_ptr<router::IShardRouter> router) {
    return std::make_shared<SqlWebSessionRepository>(std::move(router));
}
} // namespace mail_system::repository
