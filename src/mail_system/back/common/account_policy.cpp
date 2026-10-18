#include "mail_system/back/common/account_policy.h"
#include "mail_system/back/algorithm/smtp_utils.h"
namespace mail_system {
void AccountPolicy::login_async(const std::string &mail_address, const std::string &password,
                                std::function<void(AccountAuthResult)> cb, bool fresh) {
    auto router = router_;
    auto cache = cache_;
    auto worker_pool = worker_pool_;
    auto task = [mail_address, password, router, cache, fresh, worker_pool, cb = std::move(cb)]() mutable {
        AccountAuthResult out;
        out.address = mail_address;

        int shard = 0;
        if (router) {
            int r = router->route(mail_address);
            if (r >= 0)
                shard = r;
        }

        // 快路径：缓存命中（含禁用负缓存）不查 DB —— 与三协议一致
        AuthCacheEntry ce;
        if (!fresh && cache->lookup(mail_address, ce)) {
            if (ce.status != 1) {
                cb(out);
                return;
            }
            shard = ce.shard;
            if (verify_password_hash(password, ce.password_hash)) {
                out.ok = true;
                out.credential_stamp = credential_stamp(ce.password_hash);
                out.shard = ce.shard;
                out.user_id = ce.user_id; // 本服务写入缓存时带 user_id
            }
            cb(out);
            return;
        }

        auto db_pool = router ? router->get_db_pool(static_cast<size_t>(shard)) : nullptr;
        if (!db_pool) {
            LOG_AT("ACCOUNT", err, "login: no DB pool for shard {}", shard);
            out.server_error = true;
            cb(out);
            return;
        }
        auto conn = db_pool->acquire_connection();
        if (!conn->is_valid()) {
            LOG_AT("ACCOUNT", err, "login: no DB connection");
            out.server_error = true;
            cb(out);
            return;
        }

        (*conn)->async_query(
            db::sql::build_auth_user_query(), {mail_address},
            [mail_address, password, shard, conn, cache, db_pool, fresh, worker_pool, out,
             cb = std::move(cb)](std::shared_ptr<IDBResult> result) mutable {
                auto verify = [mail_address, password, shard, conn, cache, db_pool, fresh, worker_pool, out,
                               result, cb = std::move(cb)]() mutable {
                    if (!result) {
                        LOG_AT("ACCOUNT", err, "login query failed shard={}", shard);
                        out.server_error = true;
                        cb(out);
                        return;
                    }
                    if (result->get_row_count() == 0) {
                        LOG_AT("ACCOUNT", info, "login: user not found {}", mail_address);
                        cb(out); // ok=false，401
                        return;
                    }
                    // safe_stoull：status 列脏值 → 0 → 按禁用拒绝（fail-closed），
                    // 裸 std::stoi 抛异常会穿线程 terminate 整进程
                    int status = static_cast<int>(safe_stoull(result->get_value(0, "status")));
                    if (status != 1) {
                        LOG_AT("ACCOUNT", warn, "login: account disabled {}", mail_address);
                        cb(out);
                        return;
                    }
                    std::string stored = result->get_value(0, "password");
                    AuthCacheEntry entry{stored, status, safe_stoull(result->get_value(0, "id")), shard};
                    cache->store(mail_address, entry);
                    if (!verify_password_hash(password, stored)) {
                        cb(out);
                        return;
                    }

                    out.ok = true;
                    out.user_id = entry.user_id;
                    out.shard = shard;
                    out.credential_stamp = credential_stamp(stored);
                    // rehash-on-login：已是 bcrypt 直通；历史行 CAS 升级 + 刷缓存；
                    // 失败只记日志不影响本次登录（见 common/password_rehash.h）
                    rehash_password_if_needed_async(
                        conn, cache, mail_address, password, stored, entry,
                        [conn, db_pool, out, password, fresh, worker_pool, cb = std::move(cb)]() mutable {
                            auto finish = [conn, db_pool,
                                           cb = std::move(cb)](AccountAuthResult current) mutable {
                                if (!current.ok) { cb(current); return; }
                                (*conn)->async_execute(
                                    db::sql::build_update_last_login(), {current.address},
                                    [conn, db_pool, current, cb = std::move(cb)](bool ok) mutable {
                                        if (!ok)
                                            LOG_AT("ACCOUNT", warn, "last_login update failed user_id={}",
                                                   current.user_id);
                                        cb(current);
                                    });
                            };
                            if (!fresh) {
                                finish(out);
                                return;
                            }
                            // CAS rehash may lose to a concurrent reset. Re-read authoritative credentials;
                            // never mint a web session from a stale AuthCache or an old verified hash.
                            (*conn)->async_query(
                                db::sql::build_auth_user_query(), {out.address},
                                [conn, db_pool, out, password, worker_pool,
                                 finish = std::move(finish)](std::shared_ptr<IDBResult> live) mutable {
                                    auto verify = [conn, db_pool, out, password, live,
                                                   finish = std::move(finish)]() mutable {
                                        if (!live) {
                                            out.ok = false;
                                            out.server_error = true;
                                            LOG_AT("ACCOUNT", err, "credential refresh failed user_id={}",
                                                   out.user_id);
                                            finish(out);
                                            return;
                                        }
                                        if (!live->get_row_count() ||
                                            safe_stoull(live->get_value(0, "status")) != 1 ||
                                            safe_stoull(live->get_value(0, "id")) != out.user_id) {
                                            out.ok = false;
                                            finish(out);
                                            return;
                                        }
                                        auto stored = live->get_value(0, "password");
                                        auto stamp = credential_stamp(stored);
                                        if (stamp != out.credential_stamp &&
                                            !verify_password_hash(password, stored)) {
                                            out.ok = false;
                                            finish(out);
                                            return;
                                        }
                                        out.credential_stamp = stamp;
                                        finish(out);
                                    };
                                    if (worker_pool)
                                        worker_pool->post(std::move(verify));
                                    else
                                        verify();
                                });
                        });
                };
                if (worker_pool)
                    worker_pool->post(std::move(verify));
                else
                    verify();
            });
    };

    if (auto *pool = worker_pool.get()) {
        pool->post(std::move(task));
    } else {
        task(); // 无 worker 池（单测）：内联执行，回调链不挑线程
    }
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
