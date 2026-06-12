#ifndef MAIL_SYSTEM_STATIC_SHARD_ROUTER_H
#define MAIL_SYSTEM_STATIC_SHARD_ROUTER_H

#include "mail_system/back/router/i_shard_router.h"
#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mail_system {
namespace router {

// ====================================================================
// 静态分片路由器（header-only）
//
// 按 email 的域名部分映射到 shard，未匹配的域名回退到 default_shard。
// 适用于按组织/域名隔离的部署场景。
// ====================================================================
class StaticShardRouter : public IShardRouter {
public:
    StaticShardRouter(std::vector<std::pair<std::string, int>> mappings,
                      int default_shard,
                      std::vector<std::shared_ptr<DBPool>> db_pools,
                      std::vector<std::shared_ptr<storage::IStorageProvider>> storages)
        : m_default_shard(default_shard)
        , m_db_pools(std::move(db_pools))
        , m_storages(std::move(storages))
    {
        for (auto& [domain, shard] : mappings) {
            m_domain_map[to_lower(domain)] = shard;
        }
    }

    int route(const std::string& email) override {
        if (email.empty()) return m_default_shard;
        std::string domain = extract_domain(to_lower(email));
        if (domain.empty()) return m_default_shard;

        auto it = m_domain_map.find(domain);
        if (it != m_domain_map.end()) return it->second;
        return m_default_shard;
    }

    size_t shard_count() const override { return m_db_pools.size(); }
    const char* name() const override { return "static"; }

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

    static std::string extract_domain(const std::string& email) {
        auto pos = email.find('@');
        if (pos == std::string::npos || pos + 1 >= email.size()) return "";
        return email.substr(pos + 1);
    }

    std::unordered_map<std::string, int> m_domain_map;
    int m_default_shard;
    std::vector<std::shared_ptr<DBPool>> m_db_pools;
    std::vector<std::shared_ptr<storage::IStorageProvider>> m_storages;
};

} // namespace router
} // namespace mail_system

#endif
