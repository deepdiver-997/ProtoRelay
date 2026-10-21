#include "mail_system/back/common/account_policy.h"
#include "mail_system/back/common/logger.h"
#include "mail_system/back/common/safe_parse.h"
#include "mail_system/back/db/sql_queries.h"
#include "mail_system/back/algorithm/smtp_utils.h"
namespace mail_system {
void AccountPolicy::login_async(const std::string& address, const std::string& password,
                                std::function<void(AccountAuthResult)> complete, bool fresh) {
    authenticator_->login_async(address, password, std::move(complete), fresh);
}

void AccountPolicy::user_exists_async(const std::string &mail_address, std::function<void(bool)> cb) {

    int shard = 0;
    if (router_) {
        int r = router_->route(mail_address);
        if (r >= 0)
            shard = r;
    }

    // 快路径：缓存命中（含负缓存 status=0）直接回调
    AuthCacheEntry ce;
    if (recipients_->lookup(mail_address, ce)) {
        cb(ce.status == 1);
        return;
    }

    // 缓存未命中，查 DB
    std::shared_ptr<DBPool> pool = router_ ? router_->get_db_pool(static_cast<size_t>(shard)) : nullptr;
    if (!pool) {
        LOG_SMTP_WARN("RCPT check no DB pool for shard {}", shard);
        cb(false);
        return;
    }
    auto conn = pool->acquire_connection();
    if (!conn->is_valid()) {
        LOG_SMTP_WARN("RCPT check failed to get DB connection");
        cb(false);
        return;
    }

    std::string sql = db::sql::build_recipient_exists_query();
    (*conn)->async_query(sql, {mail_address},
                         [recipients = recipients_, pool, mail_address, conn,
                          cb = std::move(cb)](std::shared_ptr<IDBResult> result) mutable {
                             if (!result)
                                 LOG_AT("ACCOUNT", warn, "recipient query failed address={}", mail_address);
                             if (!result || result->get_row_count() == 0) {
                                 // 负缓存：不存在
                                 recipients->store(mail_address, {"", 0, 0, 0});
                                 cb(false);
                                 return;
                             }
                             // safe_stoull：脏 status → 0 → 负缓存 + 550 User unknown（fail-closed），不抛
                             int status = static_cast<int>(safe_stoull(result->get_value(0, "status")));
                             recipients->store(mail_address, {"", status, 0, 0});
                             cb(status == 1);
                         });
}

void AccountPolicy::alias_targets_async(const std::string &alias_address,
                                        std::function<void(std::vector<std::string>)> cb) {
    std::vector<std::string> cached;
    bool stale = false;
    if (aliases_->get(alias_address, cached, stale) && !stale) {
        cb(std::move(cached));
        return;
    }

    int shard = 0;
    if (router_) {
        int r = router_->route(alias_address);
        if (r >= 0)
            shard = r;
    }
    std::shared_ptr<DBPool> pool = router_ ? router_->get_db_pool(static_cast<size_t>(shard)) : nullptr;
    if (!pool) {
        LOG_SMTP_WARN("alias check no DB pool for shard {}", shard);
        cb({});
        return;
    }
    auto conn = pool->acquire_connection();
    if (!conn->is_valid()) {
        LOG_SMTP_WARN("alias check failed to get DB connection");
        cb({});
        return;
    }

    std::string sql = db::sql::build_alias_targets_query();
    (*conn)->async_query(sql, {alias_address},
                         [aliases = aliases_, pool, alias_address, conn,
                          cb = std::move(cb)](std::shared_ptr<IDBResult> result) mutable {
                             std::vector<std::string> targets;
                             if (!result)
                                 LOG_AT("ACCOUNT", warn, "alias query failed address={}", alias_address);
                             if (result) {
                                 for (size_t i = 0; i < result->get_row_count(); ++i)
                                     targets.push_back(result->get_value(i, "target_address"));
                                 aliases->put(alias_address, targets);
                             }
                             cb(std::move(targets));
                         });
}

void AccountPolicy::check_send_quota_async(const std::string &mail_address, int daily_limit,
                                           std::function<void(bool)> cb) {
    if (!cb)
        return;

    if (daily_limit <= 0) {
        cb(true);
        return;
    }

    int shard = 0;
    if (router_) {
        int r = router_->route(mail_address);
        if (r >= 0)
            shard = r;
    }

    std::shared_ptr<DBPool> pool = router_ ? router_->get_db_pool(static_cast<size_t>(shard)) : nullptr;
    if (!pool) {
        LOG_SMTP_WARN("quota check: no DB pool for shard {} (fail-open)", shard);
        cb(true);
        return;
    }
    auto conn = pool->acquire_connection();
    if (!conn->is_valid()) {
        LOG_SMTP_WARN("quota check: DB connection failed (fail-open)");
        cb(true);
        return;
    }

    std::string sql = db::sql::build_increment_sent_today_query();
    (*conn)->async_execute(
        sql, {mail_address, std::to_string(daily_limit)}, [conn, pool, cb = std::move(cb)](bool ok) mutable {
            if (!ok) {
                LOG_SMTP_WARN("quota check: conditional update failed (fail-open)");
                cb(true);
                return;
            }
            // 同一连接读 ROW_COUNT()：1 = 占用成功，0 = 配额已超
            (*conn)->async_query(db::sql::build_select_row_count(),
                                 [conn, pool, cb = std::move(cb)](std::shared_ptr<IDBResult> r) mutable {
                                     size_t affected = 0;
                                     if (r && r->get_row_count() > 0) {
                                         try {
                                             affected = static_cast<size_t>(
                                                 std::stoull(r->get_value(0, "affected")));
                                         } catch (...) {
                                             LOG_AT("ACCOUNT", warn, "quota affected rows invalid");
                                             affected = 0;
                                         }
                                     }
                                     if (!r || r->get_row_count() == 0)
                                         LOG_AT("ACCOUNT", warn, "quota affected rows unavailable");
                                     cb(affected > 0);
                                 });
        });
}

bool AccountPolicy::recipient_allowed(const std::string &address, const std::string &domain, bool external) {
    auto at = address.rfind('@');
    return external ||
           (at != std::string::npos &&
            algorithm::to_lower(algorithm::trim(address.substr(at + 1))) == algorithm::to_lower(domain));
}
} // namespace mail_system
