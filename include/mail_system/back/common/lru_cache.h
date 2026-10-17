#ifndef MAIL_SYSTEM_LRU_CACHE_H
#define MAIL_SYSTEM_LRU_CACHE_H

// LruCache 已上移 framework/cache（namespace pr，2026-10-18）。本文件保留
// 邮箱域类型（IMailboxCache 等）+ 向后兼容别名。
#include "framework/cache/lru_cache.h"
#include <cstdint>
#include <string>

namespace mail_system {

using pr::LruCache;

struct MailboxCacheEntry {
    uint64_t exists = 0;
    uint64_t unseen = 0;
    uint64_t uidnext = 0;
    uint64_t uidvalidity = 0;
};

inline std::string mbox_cache_key(uint64_t user_id, uint64_t mailbox_id) {
    return std::to_string(user_id) + ":" + std::to_string(mailbox_id);
}

struct MailSummary {
    uint64_t uid = 0;
    int flags = 0;
    uint64_t size = 0;
    std::string subject;
    std::string from;
    std::string date;
};

class IMailboxCache {
public:
    virtual ~IMailboxCache() = default;
    virtual void notify_change(uint64_t user_id, uint64_t mailbox_id) = 0;
};

} // namespace mail_system
#endif // MAIL_SYSTEM_LRU_CACHE_H
