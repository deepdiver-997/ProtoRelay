#ifndef MAIL_SYSTEM_HASH_SHARD_ROUTER_H
#define MAIL_SYSTEM_HASH_SHARD_ROUTER_H

#include "mail_system/back/router/i_shard_router.h"
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace mail_system {
namespace router {

// ====================================================================
// 哈希分片路由器（header-only）
//
// route(email) = hash(lower(email)) % shard_count
// 每个 shard 对应独立的 DBPool + IStorageProvider
// ====================================================================
class HashShardRouter : public IShardRouter {
public:
    HashShardRouter(size_t shard_count,
                    std::vector<std::shared_ptr<DBPool>> db_pools,
                    std::vector<std::shared_ptr<storage::IStorageProvider>> storages)
        : m_shard_count(shard_count > 0 ? shard_count : 1)
        , m_db_pools(std::move(db_pools))
        , m_storages(std::move(storages))
    {
        if (m_db_pools.size() < m_shard_count)
            m_db_pools.resize(m_shard_count, nullptr);
        if (m_storages.size() < m_shard_count)
            m_storages.resize(m_shard_count, nullptr);
    }

    int route(const std::string& email) override {
        if (email.empty()) return 0;
        std::string lower = to_lower(email);
        return static_cast<int>(std::hash<std::string>{}(lower) % m_shard_count);
    }

    size_t shard_count() const override { return m_shard_count; }
    const char* name() const override { return "hash"; }

    std::shared_ptr<DBPool> get_db_pool(size_t shard) override {
        if (shard >= m_db_pools.size()) return nullptr;
        return m_db_pools[shard];
    }

    std::shared_ptr<storage::IStorageProvider> get_storage(size_t shard) override {
        if (shard >= m_storages.size()) return nullptr;
        return m_storages[shard];
    }

private:
    static std::string to_lower(const std::string& s) {
        std::string r = s;
        std::transform(r.begin(), r.end(), r.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return r;
    }

    size_t m_shard_count;
    std::vector<std::shared_ptr<DBPool>> m_db_pools;
    std::vector<std::shared_ptr<storage::IStorageProvider>> m_storages;
};

} // namespace router
} // namespace mail_system

#endif
