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

    // 查缓存命中返回 true，填充 entry（即使 TTL 过期也会返回）
    bool lookup(const std::string& email, AuthCacheEntry& out) {
        bool stale;
        return cache_.get(email, out, stale);
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
