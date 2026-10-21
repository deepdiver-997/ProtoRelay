#ifndef MAIL_SYSTEM_PASSWORD_REHASH_H
#define MAIL_SYSTEM_PASSWORD_REHASH_H

#include "mail_system/back/common/auth_cache.h"
#include "mail_system/back/common/bcrypt.h"
#include "mail_system/back/common/logger.h"
#include "mail_system/back/repository/account_repository.h"
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace mail_system {

// rehash-on-login：验密成功后把 MD5 / 明文历史行升级为 bcrypt（SMTP/IMAP/POP3 共用）。
//
// 为什么不是一次性迁移脚本：MD5/明文不可逆，只有用户登录这一刻手里才有明文，才算得出
// bcrypt。所以升级只能随登录逐账号发生；没登录过的账号保持原样、照旧能登。管理员重置
// 密码时写 MD5 甚至明文也合法——用户下次登录自动收敛，运维习惯不变。
//
// 契约：
//   - 只在 verify_password_hash(password, stored) 已为 true 之后调用；错误密码绝不触发。
//   - stored 已是 bcrypt → 直接 done()，不碰 DB。
//   - DB 回写走 CAS（WHERE password = stored）：用户用旧密码登录中、管理员同时改了密码
//     → UPDATE 影响 0 行，不会把新密码覆盖回旧值；下次登录按新密码走。
//     残余：CAS 落空时 execute 仍可能回 ok=true（0 行不算错误），缓存会被刷成旧密码的
//     bcrypt —— 与不做 rehash 时缓存里躺着旧 stored 等价，都受同一个 TTL 窗口约束，
//     不新增暴露。
//   - 任何失败（bcrypt_hash 失败 / 无连接 / UPDATE 失败）只记日志，不影响本次登录结果；
//     历史行保持原样，下次登录再试。
//   - 成功后把缓存条目刷成新 bcrypt：否则明文/MD5 会在 AuthCache 里再躺一个 TTL。
//     缓存命中路径不升级 —— TTL 过期回源走 DB 路径时自然补上。
//   - done 恰好被调用一次；回调线程与仓储完成回调一致（真实池默认
//     内联即调用线程）。bcrypt_hash 是几十~几百 ms 纯 CPU，本函数须在 worker 线程发起
//     —— 三条认证链本来就满足（见各自 auth_user_async 注释）。
//   - 11.3 纪律：access 以拷贝捕获，不在同一调用表达式里 move。
inline void rehash_password_if_needed_async(
    std::shared_ptr<repository::IAccountCredentialAccess> access,
    std::shared_ptr<AuthCache> cache,
    const std::string& mail_address,
    const std::string& password,
    const std::string& stored,
    AuthCacheEntry entry,
    std::function<void()> done)
{
    if (!done) done = [] {};
    if (!password_needs_rehash(stored)) { done(); return; }

    std::string new_hash = bcrypt_hash(password);
    if (new_hash.empty()) {
        LOG_AUTH_WARN("rehash-on-login: bcrypt_hash failed for {}, keeping legacy hash", mail_address);
        done();
        return;
    }
    if (!access) {
        LOG_AUTH_WARN("rehash-on-login: no DB connection for {}, keeping legacy hash", mail_address);
        done();
        return;
    }

    access->upgrade_legacy_hash_async(stored, new_hash,
        [access, cache, mail_address, new_hash, entry = std::move(entry),
         done = std::move(done)](bool ok) mutable {
            if (ok) {
                if (cache) {
                    entry.password_hash = new_hash;
                    cache->store(mail_address, std::move(entry));
                }
                LOG_AUTH_INFO("rehash-on-login: {} password hash upgraded to bcrypt", mail_address);
            } else {
                LOG_AUTH_WARN("rehash-on-login: UPDATE failed for {}, keeping legacy hash (retry next login)",
                              mail_address);
            }
            done();
        });
}

} // namespace mail_system

#endif // MAIL_SYSTEM_PASSWORD_REHASH_H
