#ifndef MAIL_SYSTEM_DISTRIBUTED_MYSQL_POOL_H
#define MAIL_SYSTEM_DISTRIBUTED_MYSQL_POOL_H

#include "mail_system/back/db/db_pool.h"
#include "mail_system/back/db/mysql_pool.h"
#include <atomic>
#include <unordered_map>

namespace mail_system {

// 多节点 MySQL 连接池：对外仍返回 MySQL 兼容连接，便于复用现有 SQL 调用。
class DistributedMySQLPool : public DBPool {
public:
    DistributedMySQLPool(const DBPoolConfig& config, std::shared_ptr<DBService> db_service);
    ~DistributedMySQLPool() override;

    std::shared_ptr<IDBConnection> get_connection() override;
    void release_connection(std::shared_ptr<IDBConnection> connection) override;
    size_t get_pool_size() const override;
    size_t get_available_connections() const override;
    void close() override;

protected:
    void initialize_pool() override;
    std::shared_ptr<IDBConnection> create_connection() override;

private:
    struct NodeState {
        DBPoolConfig node_config;
        std::shared_ptr<DBPool> pool;
        std::atomic<bool> healthy;
        std::chrono::steady_clock::time_point last_failure;

        NodeState()
            : healthy(true),
              last_failure(std::chrono::steady_clock::time_point::min()) {}
    };

    DBPoolConfig m_config;
    std::shared_ptr<DBService> m_db_service;
    std::vector<std::shared_ptr<NodeState>> m_nodes;
    std::atomic<size_t> m_round_robin_cursor;
    std::atomic<bool> m_running;

    mutable std::mutex m_mutex;
    std::unordered_map<IDBConnection*, size_t> m_connection_node_map;

    size_t pick_next_node_index();
    bool can_try_node(const std::shared_ptr<NodeState>& node) const;
    void mark_node_failure(const std::shared_ptr<NodeState>& node);
    DBPoolConfig build_node_pool_config(const DBPoolConfig::NodeConfig& node) const;
};

class DistributedMySQLPoolFactory : public DBPoolFactory {
public:
    ~DistributedMySQLPoolFactory() override = default;

    std::shared_ptr<DBPool> create_pool(
        const DBPoolConfig& config,
        std::shared_ptr<DBService> db_service
    ) override;

    static DistributedMySQLPoolFactory& get_instance();

private:
    static std::unique_ptr<DistributedMySQLPoolFactory> s_instance;
    static std::mutex s_mutex;

    DistributedMySQLPoolFactory() = default;
    DistributedMySQLPoolFactory(const DistributedMySQLPoolFactory&) = delete;
    DistributedMySQLPoolFactory& operator=(const DistributedMySQLPoolFactory&) = delete;
};

} // namespace mail_system

#endif // MAIL_SYSTEM_DISTRIBUTED_MYSQL_POOL_H
