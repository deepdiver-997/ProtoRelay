#include "mail_system/back/db/mysql_pool.h"
#include "mail_system/back/common/logger.h"
#include <algorithm>
#include <iostream>

namespace mail_system {

// 静态成员初始化
std::unique_ptr<MySQLPoolFactory> MySQLPoolFactory::s_instance = nullptr;
std::mutex MySQLPoolFactory::s_mutex;

// MySQLPool实现

MySQLPool::MySQLPool(const DBPoolConfig& config, std::shared_ptr<DBService> db_service)
    : m_config(config), m_dbService(db_service), m_running(true) {
    LOG_DATABASE_DEBUG("MySQLPool constructor called");
    initialize_pool();
    m_maintenanceThread = std::thread(&MySQLPool::maintenance_thread, this);
    LOG_DATABASE_DEBUG("Maintenance thread assigned");
}

MySQLPool::~MySQLPool() {
    close();
}

void MySQLPool::execute_sql_script(const std::string& script_path) {
    std::ifstream script_file(script_path);
    if (!script_file.is_open()) {
        LOG_DATABASE_ERROR("Failed to open SQL script: {}", script_path);
        throw std::runtime_error("Failed to open SQL script");
    }

    std::stringstream buffer;
    buffer << script_file.rdbuf();
    std::string script_content = buffer.str();
    script_file.close();

    LOG_DATABASE_INFO("Executing SQL script: {}", script_path);

    // 处理 DELIMITER 命令并分割 SQL 语句
    std::vector<std::string> sql_statements;
    std::string delimiter = ";";
    std::string current_statement;
    std::string line;
    std::stringstream script_stream(script_content);

    while (std::getline(script_stream, line)) {
        // 移除行首尾空白
        size_t start = line.find_first_not_of(" \t\r\n");
        size_t end = line.find_last_not_of(" \t\r\n");
        if (start == std::string::npos) {
            // 空行或仅包含空白字符
            continue;
        }
        std::string trimmed_line = line.substr(start, end - start + 1);

        // 检查是否是 DELIMITER 命令
        if (trimmed_line.substr(0, 9) == "DELIMITER") {
            std::string new_delimiter = trimmed_line.substr(9);
            // 移除 DELIMITER 后的空白
            start = new_delimiter.find_first_not_of(" \t");
            end = new_delimiter.find_last_not_of(" \t\r\n");
            if (start != std::string::npos) {
                delimiter = new_delimiter.substr(start, end - start + 1);
                LOG_DATABASE_DEBUG("Delimiter changed to: {}", delimiter);
            }
            continue;
        }

        current_statement += line + "\n";

        // 检查是否包含当前的分隔符
        if (line.find(delimiter) != std::string::npos) {
            // 移除末尾的分隔符
            size_t pos = current_statement.rfind(delimiter);
            if (pos != std::string::npos) {
                std::string statement = current_statement.substr(0, pos);
                // 移除语句末尾的空白
                end = statement.find_last_not_of(" \t\r\n");
                if (end != std::string::npos) {
                    statement = statement.substr(0, end + 1);
                }
                if (!statement.empty()) {
                    sql_statements.push_back(statement);
                    LOG_DATABASE_DEBUG("Parsed SQL statement #{}: {}...",
                                     sql_statements.size(), statement.substr(0, 100));
                }
                current_statement.clear();
            }
        }
    }

    // 处理最后一个语句（可能没有以分隔符结尾）
    if (!current_statement.empty()) {
        auto end = current_statement.find_last_not_of(" \t\r\n");
        if (end != std::string::npos) {
            current_statement = current_statement.substr(0, end + 1);
        }
        if (!current_statement.empty()) {
            sql_statements.push_back(current_statement);
        }
    }

    LOG_DATABASE_DEBUG("Total SQL statements to execute: {}", sql_statements.size());

    // 获取一个连接并执行SQL语句
    auto connection = get_connection();
    if (!connection) {
        LOG_DATABASE_ERROR("Failed to get connection for executing SQL script");
        throw std::runtime_error("Failed to get connection for executing SQL script");
    }

    int success_count = 0;
    int failed_count = 0;

    for (size_t i = 0; i < sql_statements.size(); ++i) {
        const auto& statement = sql_statements[i];
        try {
            LOG_DATABASE_DEBUG("Executing statement #{}/{}", (i + 1), sql_statements.size());
            if (connection->execute(statement)) {
                success_count++;
                LOG_DATABASE_DEBUG("Statement #{} executed successfully", (i + 1));
            } else {
                failed_count++;
                LOG_DATABASE_ERROR("Statement #{} execution returned false", (i + 1));
                LOG_DATABASE_ERROR("Statement: {}...", statement.substr(0, 200));
            }
        } catch (const std::exception& e) {
            failed_count++;
            LOG_DATABASE_ERROR("Failed to execute SQL statement #{}\nError: {}", (i + 1), e.what());
            LOG_DATABASE_ERROR("Statement: {}...", statement.substr(0, 200));
        }
    }

    release_connection(connection);

    LOG_DATABASE_INFO("SQL script execution completed. Success: {}, Failed: {}",
                      success_count, failed_count);

    if (failed_count > 0) {
        LOG_DATABASE_WARN("{} SQL statement(s) failed", failed_count);
    }
}

void MySQLPool::initialize_pool() {
    LOG_DATABASE_DEBUG("MySQLPool::initialize_pool() called");

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // 创建初始连接
        for (size_t i = 0; i < m_config.initial_pool_size; ++i) {
            auto connection = create_connection();
            if (connection) {
                LOG_DATABASE_DEBUG("Initial connection #{} object created, now connecting to database...", (i + 1));

                // 立即连接到数据库
                if (!connection->connect()) {
                    LOG_DATABASE_ERROR("Initial connection #{} failed to connect to database", (i + 1));
                    LOG_DATABASE_ERROR("Error: {}", connection->get_last_error());
                    continue;
                }

                LOG_DATABASE_DEBUG("Initial connection #{} connected successfully", (i + 1));

                auto wrapper = std::make_shared<ConnectionWrapper>(connection);
                m_connections.push_back(wrapper);
                m_availableConnections.push(wrapper);
            }
            else {
                LOG_DATABASE_ERROR("Failed to create initial connection #{}", (i + 1));
            }
        }
    }

    // 执行SQL脚本（仅当配置了脚本路径时）
    if (!m_config.initialize_script.empty()) {
        try {
            execute_sql_script(m_config.initialize_script);
            reconnect_pool_connections();
        } catch (const std::exception& e) {
            LOG_DATABASE_ERROR("Failed to execute SQL script: {}", e.what());
            close();
            LOG_DATABASE_ERROR("Database pool closed due to initialization failure.");
            return;
        }
    }
}

void MySQLPool::reconnect_pool_connections() {
    std::lock_guard<std::mutex> lock(m_mutex);

    // 重置可用连接队列，重连成功后再重新入队。
    std::queue<std::shared_ptr<ConnectionWrapper>> empty;
    std::swap(m_availableConnections, empty);

    std::size_t success = 0;
    std::size_t failed = 0;
    for (auto& wrapper : m_connections) {
        if (!wrapper || !wrapper->connection) {
            ++failed;
            continue;
        }

        wrapper->connection->disconnect();
        if (wrapper->connection->connect()) {
            wrapper->in_use = false;
            wrapper->last_used = std::chrono::steady_clock::now();
            m_availableConnections.push(wrapper);
            ++success;
        } else {
            wrapper->in_use = false;
            ++failed;
        }
    }

    LOG_DATABASE_DEBUG("Reconnect pool connections after schema init: success={}, failed={}",
                      success,
                      failed);
}

std::shared_ptr<IDBConnection> MySQLPool::create_connection() {
    LOG_DATABASE_DEBUG("MySQLPool::create_connection() called.");
    return m_dbService->create_connection(
        m_config.host,
        m_config.user,
        m_config.password,
        m_config.database,
        m_config.port
    );
}

std::shared_ptr<IDBConnection> MySQLPool::get_connection() {
    const auto now = std::chrono::steady_clock::now();
    std::shared_ptr<ConnectionWrapper> wrapper;
    bool need_ping = false;
    bool expanding = false;   // 池耗尽超时后的扩容名额：建连挪到锁外
    size_t pool_total = 0;
    {
        std::unique_lock<std::mutex> lock(m_mutex);

        LOG_DATABASE_DEBUG("MySQLPool::get_connection() called");
        LOG_DATABASE_DEBUG("  Available connections: {}", m_availableConnections.size());
        LOG_DATABASE_DEBUG("  Total connections: {}", m_connections.size());
        LOG_DATABASE_DEBUG("  Connection timeout: {} seconds", m_config.connection_timeout);

        // 等待可用连接，最多等待连接超时时间
        auto timeout = std::chrono::seconds(m_config.connection_timeout);
        LOG_DATABASE_DEBUG("  Waiting for available connection (timeout: {}s)...", m_config.connection_timeout);
        bool hasConnection = m_cv.wait_for(lock, timeout, [this] {
            return !m_availableConnections.empty() || !m_running;
        });

        if (!m_running) {
            LOG_DATABASE_DEBUG("MySQLPool::get_connection() - pool is not running");
            return nullptr;
        }

        if (!hasConnection) {
            if (m_connections.size() >= m_config.max_pool_size) {
                LOG_DATABASE_DEBUG("MySQLPool::get_connection() - timeout waiting for connection (pool at max)");
                return nullptr;
            }
            // 扩容：先在锁内占住名额（防多线程超发），建连这一网络往返在锁外做。
            // 临界区里只允许内存操作——历史上 checkout 校验/建连都在锁内做网络 I/O，
            // 把全池借还串成了单队列（bench/imap REPORT 2026-09-22 串行点根因）。
            wrapper = std::make_shared<ConnectionWrapper>(nullptr);
            wrapper->in_use = true;
            m_connections.push_back(wrapper);
            expanding = true;
            pool_total = m_connections.size();
        } else {
            // 获取可用连接
            LOG_DATABASE_DEBUG("  Got available connection from pool");
            wrapper = m_availableConnections.front();
            m_availableConnections.pop();
            // 闲置超过 validation_interval 才值得花一趟 ping 往返确认活性；
            // 热连接免校验——checkout ping 曾是全局串行点（每次 DB 操作一次 RTT）。
            need_ping = m_config.validation_interval > 0 &&
                now - wrapper->last_used >= std::chrono::seconds(m_config.validation_interval);
            wrapper->in_use = true;
            wrapper->last_used = now;
        }
    }

    if (expanding) {
        LOG_DATABASE_DEBUG("  Creating new connection (current: {}, max: {})",
                           pool_total, m_config.max_pool_size);
        auto fresh = create_connection();
        bool ok = static_cast<bool>(fresh) && fresh->connect();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (ok) {
                wrapper->connection = fresh;
            } else {
                // 建连失败：释放名额（占位 wrapper 连接为空，不入可用队列）
                wrapper->in_use = false;
                auto it = std::find(m_connections.begin(), m_connections.end(), wrapper);
                if (it != m_connections.end()) m_connections.erase(it);
            }
        }
        if (!ok) {
            LOG_DATABASE_ERROR("  Failed to create new connection");
            if (fresh) LOG_DATABASE_ERROR("  Error: {}", fresh->get_last_error());
            return nullptr;
        }
        LOG_DATABASE_DEBUG("  New connection created and connected successfully");
        return fresh;
    }

    // —— 以下校验与失效重建都在锁外：wrapper 已标记 in_use，其他线程不会碰它 ——
    auto conn = wrapper->connection;
    // is_connected() 是本地标志（免费）：被致命错误自标记断开的连接直接换新
    bool healthy = static_cast<bool>(conn) && conn->is_connected();
    if (healthy && need_ping) {
        LOG_DATABASE_DEBUG("  Validating connection (idle > {}s)...", m_config.validation_interval);
        healthy = validate_connection(conn);
    }
    if (!healthy) {
        LOG_DATABASE_DEBUG("  Connection is invalid, creating new connection...");
        auto fresh = create_connection();
        bool ok = static_cast<bool>(fresh) && fresh->connect();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (ok) {
                wrapper->connection = fresh;
                wrapper->last_used = now;
                conn = fresh;
            } else {
                // 名额保留（wrapper 留在 m_connections），死连接置空不入可用队列；
                // 之后 get_connection 的 wait 谓词看到空队列会走扩容路径补位
                wrapper->in_use = false;
                wrapper->connection = nullptr;
                wrapper->last_used = now;
                conn = nullptr;
            }
        }
        if (!conn) {
            LOG_DATABASE_ERROR("  Failed to create new connection");
            return nullptr;
        }
        LOG_DATABASE_DEBUG("  New connection created and connected successfully");
    }

    LOG_DATABASE_DEBUG("MySQLPool::get_connection() - returning connection");
    return conn;
}

void MySQLPool::release_connection(std::shared_ptr<IDBConnection> connection) {
    if (!connection) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    // 查找连接包装器
    for (auto& wrapper : m_connections) {
        if (wrapper->connection == connection) {
            // tripwire：op 还在飞就归池 = 某条链的回调漏捕了 ScopedConnection，
            // 这条连接马上会被借给下一个使用者（协议串号/UAF）。
            // 正常链的 done 会先清 in-flight 再跑回调，不该走到这里。
            if (connection->async_in_flight()) {
                LOG_DATABASE_ERROR(
                    "MySQLPool::release_connection: async op still in flight on connection {} "
                    "—— 回调漏捕 ScopedConnection（连接在飞归池）",
                    (void*)connection.get());
            }
            wrapper->in_use = false;
            wrapper->last_used = std::chrono::steady_clock::now();
            m_availableConnections.push(wrapper);
            m_cv.notify_one();
            break;
        }
    }
}

size_t MySQLPool::get_pool_size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_connections.size();
}

size_t MySQLPool::get_available_connections() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_availableConnections.size();
}

size_t MySQLPool::get_max_pool_size() const {
    return m_config.max_pool_size;
}

size_t MySQLPool::get_active_connections() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    // 活跃连接 = 总连接数 - 可用连接数
    size_t total = m_connections.size();
    size_t avail = m_availableConnections.size();
    return total > avail ? total - avail : 0;
}

void MySQLPool::close() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running) {
            return;
        }
        m_running = false;
    }

    m_cv.notify_all();

    // 等待维护线程结束
    if (m_maintenanceThread.joinable()) {
        m_maintenanceThread.join();
    }

    // 关闭所有连接
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& wrapper : m_connections) {
        if (wrapper->connection) {
            wrapper->connection->disconnect();
        }
    }
    m_connections.clear();
    
    // 清空可用连接队列
    std::queue<std::shared_ptr<ConnectionWrapper>> empty;
    std::swap(m_availableConnections, empty);
}

void MySQLPool::maintenance_thread() {
    while (m_running) {
        // 每10秒检查一次空闲连接
        std::this_thread::sleep_for(std::chrono::seconds(10));
        cleanup_idle_connections();
    }
}

void MySQLPool::cleanup_idle_connections() {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto now = std::chrono::steady_clock::now();
    auto idleTimeout = std::chrono::seconds(m_config.idle_timeout);

    // 保留至少初始连接数量的连接
    if (m_connections.size() <= m_config.initial_pool_size) {
        return;
    }

    // 检查并关闭空闲连接
    for (auto it = m_connections.begin(); it != m_connections.end();) {
        auto& wrapper = *it;
        // connection 可能为空：失效重建失败的 wrapper 名额保留但连接置空（见 get_connection）
        if (wrapper->connection &&
            !wrapper->in_use &&
            (now - wrapper->last_used) > idleTimeout &&
            m_connections.size() > m_config.initial_pool_size) {
            
            // 从可用连接队列中移除
            std::queue<std::shared_ptr<ConnectionWrapper>> tempQueue;
            while (!m_availableConnections.empty()) {
                auto conn = m_availableConnections.front();
                m_availableConnections.pop();
                if (conn != wrapper) {
                    tempQueue.push(conn);
                }
            }
            m_availableConnections = std::move(tempQueue);

            // 断开连接并从连接池中移除
            wrapper->connection->disconnect();
            it = m_connections.erase(it);
        } else {
            ++it;
        }
    }
}

bool MySQLPool::validate_connection(std::shared_ptr<IDBConnection> connection) {
    LOG_DATABASE_DEBUG("MySQLPool::validate_connection() called");

    if (!connection->is_connected()) {
        LOG_DATABASE_DEBUG("MySQLPool::validate_connection() - connection is not connected");
        return false;
    }

    // 保活/有效性校验：走 IDBConnection::ping()。
    // 默认实现 SELECT 1（MySQL 引擎，兼容旧行为）；MariaDB 引擎覆写为 mysql_ping
    // （COM_PING，不污染 prepared stmt 状态——缓存 stmt 后 SELECT 1 不再干净）。
    try {
        LOG_DATABASE_DEBUG("MySQLPool::validate_connection() - executing ping()");
        bool is_valid = connection->ping();
        LOG_DATABASE_DEBUG("MySQLPool::validate_connection() - connection is {}", (is_valid ? "valid" : "invalid"));
        return is_valid;
    } catch (const std::exception& e) {
        LOG_DATABASE_ERROR("MySQLPool::validate_connection() - exception during validation: {}", e.what());
        return false;
    } catch (...) {
        LOG_DATABASE_ERROR("MySQLPool::validate_connection() - unknown exception during validation");
        return false;
    }
}

// MySQLPoolFactory实现

std::shared_ptr<DBPool> MySQLPoolFactory::create_pool(
    const DBPoolConfig& config,
    std::shared_ptr<DBService> db_service
) {
    LOG_DATABASE_DEBUG("std::shared_ptr<DBPool> MySQLPoolFactory::create_pool called.");
    return std::make_shared<MySQLPool>(config, db_service);
}

MySQLPoolFactory& MySQLPoolFactory::get_instance() {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_instance) {
        s_instance = std::unique_ptr<MySQLPoolFactory>(new MySQLPoolFactory());
    }
    return *s_instance;
}

} // namespace mail_system