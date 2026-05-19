#ifndef MAIL_SYSTEM_LRU_CACHE_H
#define MAIL_SYSTEM_LRU_CACHE_H

#include <chrono>
#include <list>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <functional>

namespace mail_system {

// ====================================================================
// 邮箱摘要缓存条目 —— 用于 SELECT/STATUS/NOOP 的快速计数
// ====================================================================
struct MailboxCacheEntry {
    uint64_t exists = 0;
    uint64_t unseen = 0;
    uint64_t uidnext = 0;
    uint64_t uidvalidity = 0;
};

// 缓存 key 生成："{user_id}:{mailbox_id}"
inline std::string mbox_cache_key(uint64_t user_id, uint64_t mailbox_id) {
    return std::to_string(user_id) + ":" + std::to_string(mailbox_id);
}

// ====================================================================
// 邮件摘要 —— 用于缓存 FETCH 常用属性，避免每次查三表 JOIN
// ====================================================================
struct MailSummary {
    uint64_t uid = 0;
    int flags = 0;            // \Seen(bit0), \Flagged(bit1), \Deleted(bit2)
    uint64_t size = 0;        // RFC822.SIZE
    std::string subject;
    std::string from;
    std::string date;         // INTERNALDATE (RFC 3501 format)
};

// ====================================================================
// 邮箱变化通知接口
// SMTP 投递完成后通过此接口通知缓存层"某用户的某邮箱有变化"
// IMAP 侧实现此接口并注入到 SMTP，实现零耦合的缓存失效
// ====================================================================
class IMailboxCache {
public:
    virtual ~IMailboxCache() = default;

    // 通知某用户的某个邮箱有变化（新邮件 / 删改）
    virtual void notify_change(uint64_t user_id, uint64_t mailbox_id) = 0;
};

// ====================================================================
// 通用线程安全 LRU 缓存（header-only）
//
// 特性：
//   - TTL 过期返回旧数据 + 标记 stale
//   - stale-while-revalidate 友好：调用方判断 stale 后异步回源
//   - 读多写少场景使用 shared_mutex，读锁不互斥
//   - 超过 capacity 自动淘汰最久未访问条目
// ====================================================================
template <typename Key, typename Value>
class LruCache {
public:
    using Clock = std::chrono::steady_clock;

    explicit LruCache(size_t capacity, std::chrono::seconds ttl = std::chrono::seconds(5))
        : m_capacity(capacity > 0 ? capacity : 1)
        , m_ttl(ttl) {}

    // 获取缓存值。
    // 返回 true 表示缓存命中，stale 表示 TTL 已过期（值仍可用但建议回源刷新）
    bool get(const Key& key, Value& out_value, bool& out_stale) const {
        std::shared_lock lock(m_mutex);
        auto it = m_map.find(key);
        if (it == m_map.end()) {
            out_stale = false;
            return false;
        }

        // 移到链表前端（最近访问）
        m_list.splice(m_list.begin(), m_list, it->second);
        auto& entry = it->second->second;
        out_value = entry.value;
        auto age = Clock::now() - entry.created;
        out_stale = (age >= m_ttl);
        return true;
    }

    // 放入缓存
    void put(const Key& key, const Value& value) {
        std::unique_lock lock(m_mutex);
        auto it = m_map.find(key);
        if (it != m_map.end()) {
            it->second->second = {value, Clock::now()};
            m_list.splice(m_list.begin(), m_list, it->second);
            return;
        }

        if (m_map.size() >= m_capacity) {
            auto& oldest = m_list.back();
            m_map.erase(oldest.first);
            m_list.pop_back();
        }

        m_list.emplace_front(key, InternalEntry{value, Clock::now()});
        m_map[key] = m_list.begin();
    }

    // 删除指定 key
    void invalidate(const Key& key) {
        std::unique_lock lock(m_mutex);
        auto it = m_map.find(key);
        if (it != m_map.end()) {
            m_list.erase(it->second);
            m_map.erase(it);
        }
    }

    // 批量删除符合条件的条目（如某用户的全部邮箱缓存）
    template <typename Pred>
    void invalidate_if(Pred pred) {
        std::unique_lock lock(m_mutex);
        for (auto it = m_list.begin(); it != m_list.end(); ) {
            if (pred(it->first)) {
                m_map.erase(it->first);
                it = m_list.erase(it);
            } else {
                ++it;
            }
        }
    }

    // 清空缓存
    void clear() {
        std::unique_lock lock(m_mutex);
        m_list.clear();
        m_map.clear();
    }

    // 当前条目数
    size_t size() const {
        std::shared_lock lock(m_mutex);
        return m_map.size();
    }

    // 获取或刷新：如果缓存命中（含 stale）直接返回，
    // 否则调用 loader 回源加载后写入缓存再返回
    // loader 只在缓存未命中时调用（stale 时用旧值）
    Value get_or_refresh(const Key& key, std::function<Value()> loader, bool& out_stale) {
        Value v;
        if (get(key, v, out_stale)) {
            return v;
        }
        // 未命中：同步加载
        v = loader();
        put(key, v);
        out_stale = false;
        return v;
    }

private:
    struct InternalEntry {
        Value value;
        Clock::time_point created;
    };

    size_t m_capacity;
    std::chrono::seconds m_ttl;

    // 链表：前端是最近访问，后端是最久未访问
    mutable std::list<std::pair<Key, InternalEntry>> m_list;
    mutable std::unordered_map<Key, decltype(m_list.begin())> m_map;

    // 读写锁：读操作不互斥
    mutable std::shared_mutex m_mutex;
};

} // namespace mail_system

#endif // MAIL_SYSTEM_LRU_CACHE_H
