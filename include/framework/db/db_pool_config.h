#ifndef PR_FRAMEWORK_DB_DB_POOL_CONFIG_H
#define PR_FRAMEWORK_DB_DB_POOL_CONFIG_H

// DBPoolConfig — 连接池配置（框架层，2026-10-17 从 mail_system/back/db/db_pool.h 上移）。
// 它配置的是 pr::DBPool 工厂的参数（节点/端口/池大小/JSON 加载），与 pr::DBPool 同层；
// 此前留在应用 shim 头里，迫使框架的 server_config.h 反向 include 应用层。

#include "framework/log/log_macros.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace pr {

struct DBPoolConfig {
    struct NodeConfig {
        std::string name, host, user, password, database;
        unsigned int port = 3306;
        size_t weight = 1;
        bool enabled = true;
    };

    std::string achieve, host, user, password, database, initialize_script;
    unsigned int port = 3306;
    size_t initial_pool_size = 5, max_pool_size = 10;
    unsigned int connection_timeout = 5, idle_timeout = 60;
    // checkout 校验阈值（秒）：连接闲置超过该值才在 checkout 时 ping 一次活性；
    // 闲置内的热连接免 ping——checkout ping 曾在池锁内执行，把全池借还串成单队列
    // （bench/imap REPORT 2026-09-22）。0 = 每次 checkout 都 ping（旧行为）。
    // 配套：连接级致命错误会把连接自标记断开，checkout 的本地标志检查即可换新，
    // 死连接不必等下一次 ping 才被发现。
    unsigned int validation_interval = 60;
    unsigned int distributed_node_retry_interval = 5;
    std::vector<NodeConfig> nodes;

    void show() const {
        LOG_DATABASE_INFO("DBPoolConfig: achieve={} host={} user={} db={} port={}"
                          " initial_pool={} max_pool={} conn_timeout={} idle_timeout={}"
                          " validation_interval={} distributed_retry={} node_count={}",
                          achieve, host, user, database, port,
                          initial_pool_size, max_pool_size, connection_timeout,
                          idle_timeout, validation_interval,
                          distributed_node_retry_interval, nodes.size());
        for (size_t i = 0; i < nodes.size(); ++i) {
            auto& node = nodes[i];
            LOG_DATABASE_INFO("  node[{}] name={} host={} port={} db={} weight={} enabled={}",
                              i, node.name, node.host, node.port,
                              node.database, node.weight, node.enabled);
        }
    }

    bool loadFromJson(const std::string& filename) {
        std::ifstream f(filename);
        if (!f.is_open()) { LOG_DATABASE_ERROR("Failed to open config file: {}", filename); return false; }
        nlohmann::json j; f >> j;
        if (j.is_discarded()) { LOG_DATABASE_ERROR("Failed to parse config file: {}", filename); return false; }

        achieve          = j.value("achieve", achieve);
        host             = j.value("host", host);
        user             = j.value("user", user);
        password         = j.value("password", password);
        database         = j.value("database", database);
        port             = j.value("port", port);
        initial_pool_size= j.value("initial_pool_size", initial_pool_size);
        max_pool_size    = j.value("max_pool_size", max_pool_size);
        connection_timeout=j.value("connection_timeout", connection_timeout);
        idle_timeout     = j.value("idle_timeout", idle_timeout);
        validation_interval = j.value("validation_interval", validation_interval);
        distributed_node_retry_interval = j.value("distributed_node_retry_interval", distributed_node_retry_interval);

        auto resolve = [&](const std::string& rel) {
            if (rel.empty()) return std::string();
            return (std::filesystem::path(filename).parent_path() / rel).lexically_normal().string();
        };
        initialize_script = resolve(j.value("initialize_script", initialize_script));

        if (j.contains("nodes") && j["nodes"].is_array()) {
            nodes.clear();
            for (auto& item : j["nodes"]) {
                if (!item.is_object()) continue;
                NodeConfig n;
                n.name     = item.value("name", std::string());
                n.host     = item.value("host", host);
                n.user     = item.value("user", user);
                n.password = item.value("password", password);
                n.database = item.value("database", database);
                n.port     = item.value("port", port);
                n.weight   = item.value("weight", size_t(1));
                n.enabled  = item.value("enabled", true);
                if (!n.host.empty() && n.enabled) nodes.push_back(n);
            }
        }
        return true;
    }
};

} // namespace pr

#endif // PR_FRAMEWORK_DB_DB_POOL_CONFIG_H
