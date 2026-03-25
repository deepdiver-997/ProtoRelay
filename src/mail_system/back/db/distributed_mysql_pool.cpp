#include "mail_system/back/db/distributed_mysql_pool.h"
#include "mail_system/back/common/logger.h"

namespace mail_system {

std::unique_ptr<DistributedMySQLPoolFactory> DistributedMySQLPoolFactory::s_instance = nullptr;
std::mutex DistributedMySQLPoolFactory::s_mutex;

DistributedMySQLPool::DistributedMySQLPool(
    const DBPoolConfig& config,
    std::shared_ptr<DBService> db_service)
    : m_config(config),
      m_db_service(std::move(db_service)),
      m_round_robin_cursor(0),
      m_running(true) {
    initialize_pool();
}

DistributedMySQLPool::~DistributedMySQLPool() {
    close();
}

void DistributedMySQLPool::initialize_pool() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_nodes.empty()) {
        return;
    }

    if (!m_config.nodes.empty()) {
        for (const auto& node_cfg : m_config.nodes) {
            if (!node_cfg.enabled) {
                continue;
            }

            auto node = std::make_shared<NodeState>();
            node->node_config = build_node_pool_config(node_cfg);
            node->pool = MySQLPoolFactory::get_instance().create_pool(node->node_config, m_db_service);
            node->healthy.store(node->pool != nullptr);
            m_nodes.push_back(node);
        }
    }

    if (m_nodes.empty()) {
        auto node = std::make_shared<NodeState>();
        node->node_config = m_config;
        node->pool = MySQLPoolFactory::get_instance().create_pool(node->node_config, m_db_service);
        node->healthy.store(node->pool != nullptr);
        m_nodes.push_back(node);
    }

    if (m_nodes.empty()) {
        throw std::runtime_error("No available MySQL nodes for distributed pool");
    }

    LOG_DATABASE_INFO("DistributedMySQLPool initialized with {} node(s)", m_nodes.size());
}

std::shared_ptr<IDBConnection> DistributedMySQLPool::create_connection() {
    return m_db_service->create_connection(
        m_config.host,
        m_config.user,
        m_config.password,
        m_config.database,
        m_config.port
    );
}

size_t DistributedMySQLPool::pick_next_node_index() {
    return m_round_robin_cursor.fetch_add(1) % m_nodes.size();
}

bool DistributedMySQLPool::can_try_node(const std::shared_ptr<NodeState>& node) const {
    if (node->healthy.load()) {
        return true;
    }

    auto now = std::chrono::steady_clock::now();
    auto cooldown = std::chrono::seconds(m_config.distributed_node_retry_interval);
    return (now - node->last_failure) >= cooldown;
}

void DistributedMySQLPool::mark_node_failure(const std::shared_ptr<NodeState>& node) {
    node->healthy.store(false);
    node->last_failure = std::chrono::steady_clock::now();
}

std::shared_ptr<IDBConnection> DistributedMySQLPool::get_connection() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_running || m_nodes.empty()) {
        return nullptr;
    }

    const size_t node_count = m_nodes.size();
    const size_t start = pick_next_node_index();

    for (size_t offset = 0; offset < node_count; ++offset) {
        const size_t idx = (start + offset) % node_count;
        auto& node = m_nodes[idx];

        if (!node || !node->pool || !can_try_node(node)) {
            continue;
        }

        auto conn = node->pool->get_connection();
        if (conn) {
            node->healthy.store(true);
            m_connection_node_map[conn.get()] = idx;
            return conn;
        }

        mark_node_failure(node);
    }

    return nullptr;
}

void DistributedMySQLPool::release_connection(std::shared_ptr<IDBConnection> connection) {
    if (!connection) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_connection_node_map.find(connection.get());
    if (it == m_connection_node_map.end()) {
        return;
    }

    const size_t node_index = it->second;
    m_connection_node_map.erase(it);

    if (node_index < m_nodes.size() && m_nodes[node_index] && m_nodes[node_index]->pool) {
        m_nodes[node_index]->pool->release_connection(connection);
    }
}

size_t DistributedMySQLPool::get_pool_size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t total = 0;
    for (const auto& node : m_nodes) {
        if (node && node->pool) {
            total += node->pool->get_pool_size();
        }
    }
    return total;
}

size_t DistributedMySQLPool::get_available_connections() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t total = 0;
    for (const auto& node : m_nodes) {
        if (node && node->pool) {
            total += node->pool->get_available_connections();
        }
    }
    return total;
}

void DistributedMySQLPool::close() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_running) {
        return;
    }

    m_running = false;
    m_connection_node_map.clear();

    for (auto& node : m_nodes) {
        if (node && node->pool) {
            node->pool->close();
        }
    }

    m_nodes.clear();
}

DBPoolConfig DistributedMySQLPool::build_node_pool_config(const DBPoolConfig::NodeConfig& node) const {
    DBPoolConfig cfg = m_config;
    cfg.host = node.host.empty() ? m_config.host : node.host;
    cfg.user = node.user.empty() ? m_config.user : node.user;
    cfg.password = node.password.empty() ? m_config.password : node.password;
    cfg.database = node.database.empty() ? m_config.database : node.database;
    cfg.port = node.port;
    cfg.nodes.clear();
    return cfg;
}

std::shared_ptr<DBPool> DistributedMySQLPoolFactory::create_pool(
    const DBPoolConfig& config,
    std::shared_ptr<DBService> db_service
) {
    return std::make_shared<DistributedMySQLPool>(config, std::move(db_service));
}

DistributedMySQLPoolFactory& DistributedMySQLPoolFactory::get_instance() {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_instance) {
        s_instance = std::unique_ptr<DistributedMySQLPoolFactory>(new DistributedMySQLPoolFactory());
    }
    return *s_instance;
}

} // namespace mail_system
