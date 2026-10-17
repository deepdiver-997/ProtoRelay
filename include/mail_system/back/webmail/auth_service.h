#ifndef MAIL_SYSTEM_WEBMAIL_AUTH_SERVICE_H
#define MAIL_SYSTEM_WEBMAIL_AUTH_SERVICE_H

// ================================================================
// AuthService — webmail 登录编排（kickoff 设计决策 2）
//
// 第四个前端和前三个同口径：AuthCache 命中不查 DB；未命中查 users
// （build_auth_user_query）→ verify_password_hash → rehash-on-login
// （MD5/明文历史行升级 bcrypt，CAS 回写）→ update_last_login。
// 禁止自写哈希判断（kickoff 关键契约）——本文件只做编排，不碰哈希。
// ================================================================

#include "framework/thread_pool/thread_pool_base.h"
#include "mail_system/back/common/auth_cache.h"
#include "mail_system/back/common/password_rehash.h"
#include "mail_system/back/common/safe_parse.h"
#include "mail_system/back/db/db_pool.h"
#include "mail_system/back/db/sql_queries.h"
#include "mail_system/back/router/i_shard_router.h"

#include <functional>
#include <memory>
#include <string>

namespace mail_system::webmail {

struct AuthOutcome {
    bool ok = false;
    // true → 503（DB 不可用，可重试）；false 且 !ok → 401（凭据错/账号禁用，
    // 与三协议一样不区分原因，避免给未认证调用方提供账号枚举通道）。
    bool server_error = false;
    uint64_t user_id = 0;
    std::string address;
};

class AuthService {
public:
    AuthService(std::shared_ptr<router::IShardRouter> shard_router,
                std::shared_ptr<AuthCache> auth_cache,
                std::shared_ptr<pr::ThreadPoolBase> worker_pool)
        : router_(std::move(shard_router)),
          cache_(std::move(auth_cache)),
          worker_pool_(std::move(worker_pool)) {}

    // 整段认证（缓存 / DB 查询 / 验密 / rehash / last_login）作为一个任务丢给
    // worker 线程池：bcrypt 是几十~几百 ms 纯 CPU，绝不占共享 io 线程——与
    // SMTP auth_user_async 同一形状。回调在 worker 线程触发；HTTP 侧的
    // complete → do_async_write 经 exec_ctx_ 串行化，跨线程安全。
    void login_async(const std::string& mail_address, const std::string& password,
                     std::function<void(AuthOutcome)> cb) {
        auto router = router_;
        auto cache = cache_;
        auto worker_pool = worker_pool_;
        auto task = [mail_address, password, router, cache,
                     cb = std::move(cb)]() mutable {
            AuthOutcome out;
            out.address = mail_address;

            int shard = 0;
            if (router) { int r = router->route(mail_address); if (r >= 0) shard = r; }

            // 快路径：缓存命中（含禁用负缓存）不查 DB —— 与三协议一致
            AuthCacheEntry ce;
            if (cache->lookup(mail_address, ce)) {
                if (ce.status != 1) { cb(out); return; }
                shard = ce.shard;
                if (verify_password_hash(password, ce.password_hash)) {
                    out.ok = true;
                    out.user_id = ce.user_id;   // 本服务写入缓存时带 user_id
                }
                cb(out);
                return;
            }

            auto db_pool = router ? router->get_db_pool(static_cast<size_t>(shard)) : nullptr;
            if (!db_pool) { LOG_AT("WEBAPI", err, "login: no DB pool for shard {}", shard); out.server_error = true; cb(out); return; }
            auto conn = db_pool->acquire_connection();
            if (!conn->is_valid()) { LOG_AT("WEBAPI", err, "login: no DB connection"); out.server_error = true; cb(out); return; }

            (*conn)->async_query(db::sql::build_auth_user_query(), {mail_address},
                [mail_address, password, shard, conn, cache,
                 out, cb = std::move(cb)](std::shared_ptr<IDBResult> result) mutable {
                    if (!result || result->get_row_count() == 0) {
                        LOG_AT("WEBAPI", info, "login: user not found {}", mail_address);
                        cb(out);   // ok=false，401
                        return;
                    }
                    // safe_stoull：status 列脏值 → 0 → 按禁用拒绝（fail-closed），
                    // 裸 std::stoi 抛异常会穿线程 terminate 整进程
                    int status = static_cast<int>(safe_stoull(result->get_value(0, "status")));
                    if (status != 1) {
                        LOG_AT("WEBAPI", warn, "login: account disabled {}", mail_address);
                        cb(out);
                        return;
                    }
                    std::string stored = result->get_value(0, "password");
                    AuthCacheEntry entry{stored, status,
                                         safe_stoull(result->get_value(0, "id")), shard};
                    cache->store(mail_address, entry);
                    if (!verify_password_hash(password, stored)) { cb(out); return; }

                    out.ok = true;
                    out.user_id = entry.user_id;
                    // rehash-on-login：已是 bcrypt 直通；历史行 CAS 升级 + 刷缓存；
                    // 失败只记日志不影响本次登录（见 common/password_rehash.h）
                    rehash_password_if_needed_async(conn, cache, mail_address, password,
                        stored, entry,
                        [conn, out, cb = std::move(cb)]() mutable {
                            (*conn)->async_execute(db::sql::build_update_last_login(), {out.address},
                                [out, cb = std::move(cb)](bool) mutable { cb(out); });
                        });
                });
        };

        if (auto* pool = worker_pool.get()) {
            pool->post(std::move(task));
        } else {
            task();   // 无 worker 池（单测）：内联执行，回调链不挑线程
        }
    }

private:
    std::shared_ptr<router::IShardRouter> router_;
    std::shared_ptr<AuthCache> cache_;
    std::shared_ptr<pr::ThreadPoolBase> worker_pool_;
};

} // namespace mail_system::webmail

#endif // MAIL_SYSTEM_WEBMAIL_AUTH_SERVICE_H
