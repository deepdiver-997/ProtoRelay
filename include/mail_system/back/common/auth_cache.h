#ifndef MAIL_SYSTEM_AUTH_CACHE_H
#define MAIL_SYSTEM_AUTH_CACHE_H

#include "mail_system/back/common/lru_cache.h"
#include <chrono>
#include <memory>
#include <string>

namespace mail_system {

struct AuthCacheEntry {
    std::string password_hash;
    int status = 1;      // 1 = active
    uint64_t user_id = 0;
    int shard = 0;
};

// SMTP/IMAP 共享的认证缓存 — 避免同一用户多次查 DB
class AuthCache {
public:
    AuthCache(size_t capacity = 10000,
              std::chrono::seconds ttl = std::chrono::minutes(5))
        : cache_(capacity, ttl) {}

    // 查缓存：命中且未过期返回 true。过期条目视为 miss（TTL 兜底）：
    // 2026-09-28 前的实现无视 stale 标志，过期条目在被 LRU 挤掉前一直生效，
    // DB 换密码/解禁账号必须重启进程才生效（SMTP/IMAP 两个进程各一份缓存）。
    // 过期后走 DB 路径重查并回填（LruCache::put 会刷新时间戳），代价是
    // 每账号每 TTL 至多一次 DB 查询。
    bool lookup(const std::string& email, AuthCacheEntry& out) {
        bool stale;
        if (!cache_.get(email, out, stale)) return false;
        return !stale;
    }

    // 写入缓存
    void store(const std::string& email, AuthCacheEntry entry) {
        cache_.put(email, std::move(entry));
    }

    // 测试用注入（直接写入，不检查）
    void inject(const std::string& email, AuthCacheEntry entry) {
        cache_.put(email, std::move(entry));
    }

private:
    LruCache<std::string, AuthCacheEntry> cache_;
};

} // namespace mail_system
#endif
