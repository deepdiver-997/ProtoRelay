#include "mail_system/back/common/account_authentication.h"
#include "mail_system/back/common/credential_stamp.h"
#include "mail_system/back/common/password_rehash.h"
#include "mail_system/back/common/safe_parse.h"
#include "mail_system/back/db/sql_queries.h"

namespace mail_system {
void AccountAuthenticationService::login_async(const std::string &mail_address, const std::string &password,
                                std::function<void(AccountAuthResult)> cb, bool fresh) {
    if (!cb) return;
    auto router = router_;
    auto cache = cache_;
    auto worker_pool = workers_;
    auto task = [mail_address, password, router, cache, fresh, worker_pool, cb = std::move(cb)]() mutable {
        AccountAuthResult out;
        out.address = mail_address;

        int shard = 0;
        if (router) {
            int r = router->route(mail_address);
            if (r >= 0)
                shard = r;
        }

        out.shard = shard;

        // 快路径：缓存命中（含禁用负缓存）不查 DB —— 与三协议一致
        AuthCacheEntry ce;
        if (!fresh && cache && cache->lookup(mail_address, ce)) {
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
                    if (cache) cache->store(mail_address, entry);
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

} // namespace mail_system
