#include "mail_system/back/db/sqlite_service.h"
#include "framework/db/async_db_adapter.h"
#include "mail_system/back/common/logger.h"
#include <cctype>
#include <sqlite3.h>
#include <sstream>
#include <utility>

namespace mail_system {

namespace {

// sqlite3 返回码 → 人类可读（errmsg 已含主信息，这里补充语义类别进日志）。
const char* sqlite_rc_name(int rc) {
    switch (rc) {
        case SQLITE_OK:      return "OK";
        case SQLITE_BUSY:    return "BUSY";
        case SQLITE_LOCKED:  return "LOCKED";
        case SQLITE_READONLY:return "READONLY";
        case SQLITE_IOERR:   return "IOERR";
        case SQLITE_CORRUPT: return "CORRUPT";
        case SQLITE_FULL:    return "FULL";
        case SQLITE_CANTOPEN:return "CANTOPEN";
        case SQLITE_CONSTRAINT: return "CONSTRAINT";
        default:             return "ERROR";
    }
}

// 语句首词（大写）。用于 execute 路径的事务状态归一：
// 调用方绕过 begin_transaction/commit/rollback 直接跑裸 BEGIN/COMMIT/ROLLBACK 时，
// m_inTransaction 也必须保持真实，否则防御性检查会误判。
std::string leading_keyword(const std::string& sql) {
    size_t i = 0;
    while (i < sql.size() && std::isspace(static_cast<unsigned char>(sql[i]))) ++i;
    // 跳过行内注释与块注释前导
    while (i + 1 < sql.size() && sql[i] == '-' && sql[i + 1] == '-') {
        while (i < sql.size() && sql[i] != '\n') ++i;
        while (i < sql.size() && std::isspace(static_cast<unsigned char>(sql[i]))) ++i;
    }
    const size_t start = i;
    while (i < sql.size() && std::isalpha(static_cast<unsigned char>(sql[i]))) ++i;
    std::string word = sql.substr(start, i - start);
    for (auto& c : word) c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
    return word;
}

// 落盘强度降级检测：显式配置允许，但不允许静默——open 时必须留下 WARN。
void warn_if_durability_downgraded(const SQLiteConfig& cfg, const std::string& path) {
    std::string jm = cfg.journal_mode;
    for (auto& c : jm) c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
    if (jm == "OFF" || jm == "MEMORY") {
        LOG_DATABASE_WARN("SQLiteConnection: journal_mode={} explicitly disables rollback/"
                          "WAL journaling — power loss may corrupt {} (configured, not silent)",
                          cfg.journal_mode, path);
    }
    std::string syn = cfg.synchronous;
    for (auto& c : syn) c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
    if (syn == "OFF") {
        LOG_DATABASE_WARN("SQLiteConnection: synchronous=OFF explicitly disables fsync — "
                          "recent commits may be lost on power failure of {} (configured, not silent)",
                          path);
    }
}

} // namespace

// ====================================================================
// SQLiteResult
// ====================================================================

SQLiteResult::SQLiteResult(std::vector<std::string> column_names,
                           std::vector<std::vector<std::string>> rows)
    : m_columnNames(std::move(column_names)), m_rows(std::move(rows)) {}

size_t SQLiteResult::get_row_count() const { return m_rows.size(); }
size_t SQLiteResult::get_column_count() const { return m_columnNames.size(); }
std::vector<std::string> SQLiteResult::get_column_names() const { return m_columnNames; }

std::map<std::string, std::string> SQLiteResult::get_row(size_t row_index) const {
    std::map<std::string, std::string> row;
    if (row_index >= m_rows.size()) return row;
    for (size_t i = 0; i < m_columnNames.size() && i < m_rows[row_index].size(); ++i) {
        row[m_columnNames[i]] = m_rows[row_index][i];
    }
    return row;
}

std::vector<std::map<std::string, std::string>> SQLiteResult::get_all_rows() const {
    std::vector<std::map<std::string, std::string>> all;
    all.reserve(m_rows.size());
    for (size_t i = 0; i < m_rows.size(); ++i) all.push_back(get_row(i));
    return all;
}

std::string SQLiteResult::get_value(size_t row_index, const std::string& column_name) const {
    if (row_index >= m_rows.size()) return {};
    for (size_t i = 0; i < m_columnNames.size() && i < m_rows[row_index].size(); ++i) {
        if (m_columnNames[i] == column_name) return m_rows[row_index][i];
    }
    return {};
}

// ====================================================================
// SQLiteConnection
// ====================================================================

SQLiteConnection::SQLiteConnection(SQLiteConfig config) : m_config(std::move(config)) {}

SQLiteConnection::~SQLiteConnection() {
    disconnect();
}

void SQLiteConnection::set_connection_params(const std::string& database) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_path = database;   // database = 本地库文件路径；host/user/password/port 无意义
}

bool SQLiteConnection::connect() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_connected && m_db) return true;
    if (m_path.empty()) {
        LOG_DATABASE_ERROR("SQLiteConnection::connect: no database file path set");
        return false;
    }

    sqlite3* db = nullptr;
    // SQLITE_OPEN_FULLMUTEX：句柄级串行化兜底（驱动自身已用 m_mutex 单飞行，
    // 这里防调用方绕过接口直接持有句柄的多线程误用——SQLite 默认 SERIALIZED
    // 编译下等价，显式声明不依赖编译选项）。
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    const int rc = sqlite3_open_v2(m_path.c_str(), &db, flags, nullptr);
    if (rc != SQLITE_OK) {
        // open 失败也可能分配了句柄（ errmsg 挂在句柄上），先取再关。
        const char* msg = db ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
        LOG_DATABASE_ERROR("SQLiteConnection::connect: open '{}' failed ({}: {})",
                           m_path, sqlite_rc_name(rc), msg ? msg : "unknown");
        if (db) sqlite3_close(db);
        return false;
    }
    m_db = db;

    // ---- 显式配置的连接行为；PRAGMA 失败视为 connect 失败（配置必须生效，不许吞）----
    auto apply = [&](const char* pragma, const std::string& value) -> bool {
        if (value.empty()) return true;   // 空串 = 不下发，沿用库文件/编译默认
        const std::string sql = std::string("PRAGMA ") + pragma + " = " + value + ";";
        char* err = nullptr;
        const int prc = sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, &err);
        if (prc != SQLITE_OK) {
            LOG_DATABASE_ERROR("SQLiteConnection::connect: {} failed ({}: {})",
                               sql, sqlite_rc_name(prc), err ? err : "unknown");
            sqlite3_free(err);
            return false;
        }
        return true;
    };

    if (!apply("journal_mode", m_config.journal_mode) ||
        !apply("synchronous", m_config.synchronous) ||
        !apply("foreign_keys", m_config.foreign_keys ? "ON" : "OFF")) {
        sqlite3_close(m_db);
        m_db = nullptr;
        return false;
    }
    // 有界 SQLITE_BUSY 等待：库内部带 deadline 重试，超出即返回 SQLITE_BUSY。
    // 0 = 不等待立即失败；上限就是配置值，不存在无界重试。
    sqlite3_busy_timeout(m_db, m_config.busy_timeout_ms);

    warn_if_durability_downgraded(m_config, m_path);
    m_connected = true;
    LOG_DATABASE_INFO("SQLiteConnection::connect: '{}' opened (journal={} synchronous={} "
                      "busy_timeout_ms={} foreign_keys={})",
                      m_path,
                      m_config.journal_mode.empty() ? "(default)" : m_config.journal_mode,
                      m_config.synchronous.empty() ? "(default)" : m_config.synchronous,
                      m_config.busy_timeout_ms, m_config.foreign_keys);
    return true;
}

void SQLiteConnection::disconnect() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_db) {
        // 未提交的事务由 SQLite 语义回滚；显式记录便于发现调用方漏 commit。
        if (m_inTransaction) {
            LOG_DATABASE_WARN("SQLiteConnection::disconnect: closing '{}' inside an open "
                              "transaction — SQLite rolls it back", m_path);
        }
        sqlite3_close_v2(m_db);   // v2：句柄被遗留 stmt 引用时延迟释放，不失败
        m_db = nullptr;
    }
    m_connected = false;
    m_inTransaction = false;
}

bool SQLiteConnection::is_connected() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_connected && m_db != nullptr;
}

std::string SQLiteConnection::get_last_error() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db) return "not connected";
    const char* msg = sqlite3_errmsg(m_db);
    return msg ? msg : "unknown";
}

std::string SQLiteConnection::escape_string(const std::string& str) const {
    // SQLite 字面量只有单引号需要翻倍（无反斜杠转义）。驱动路径首选 ? 绑定参数，
    // 本函数仅为拼 SQL 的兜底场景提供正确语义。
    std::string out;
    out.reserve(str.size() + 8);
    for (char c : str) {
        if (c == '\'') out += "''";
        else out += c;
    }
    return out;
}

std::shared_ptr<IDBResult> SQLiteConnection::query(const std::string& sql) {
    return query(sql, {});
}

std::shared_ptr<IDBResult> SQLiteConnection::query(const std::string& sql,
                                                   const std::vector<std::string>& params) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return query_locked(sql, params);
}

std::shared_ptr<IDBResult> SQLiteConnection::query_locked(
    const std::string& sql, const std::vector<std::string>& params) {
    if (!m_connected || !m_db) {
        LOG_DATABASE_ERROR("SQLiteConnection::query: not connected ({})", m_path);
        return nullptr;
    }

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(m_db, sql.c_str(), static_cast<int>(sql.size()), &stmt, nullptr);
    if (rc != SQLITE_OK) {
        LOG_DB_QUERY_ERROR("SQLite prepare error ({}: {}): {}",
                           sqlite_rc_name(rc), sqlite3_errmsg(m_db), sql);
        return nullptr;
    }

    // 全部按 TEXT 绑定（与 MySQL/MariaDB 驱动的 string 参数语义对齐）。
    for (size_t i = 0; i < params.size(); ++i) {
        rc = sqlite3_bind_text(stmt, static_cast<int>(i + 1),
                               params[i].c_str(), static_cast<int>(params[i].size()),
                               SQLITE_TRANSIENT);
        if (rc != SQLITE_OK) {
            LOG_DB_QUERY_ERROR("SQLite bind error ({}: {}): {}",
                               sqlite_rc_name(rc), sqlite3_errmsg(m_db), sql);
            sqlite3_finalize(stmt);
            return nullptr;
        }
    }

    std::vector<std::string> column_names;
    std::vector<std::vector<std::string>> rows;
    bool header_done = false;

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const int cols = sqlite3_column_count(stmt);
        if (!header_done) {
            column_names.reserve(static_cast<size_t>(cols));
            for (int c = 0; c < cols; ++c) {
                const char* name = sqlite3_column_name(stmt, c);
                column_names.emplace_back(name ? name : "");
            }
            header_done = true;
        }
        std::vector<std::string> row(static_cast<size_t>(cols));
        for (int c = 0; c < cols; ++c) {
            // 动态类型统一文本化（NULL → 空串），与 string 结果接口语义一致
            if (sqlite3_column_type(stmt, c) != SQLITE_NULL) {
                const unsigned char* text = sqlite3_column_text(stmt, c);
                if (text) row[static_cast<size_t>(c)] =
                    reinterpret_cast<const char*>(text);
            }
        }
        rows.push_back(std::move(row));
    }

    if (rc != SQLITE_DONE) {
        LOG_DB_QUERY_ERROR("SQLite step error ({}: {}): {}",
                           sqlite_rc_name(rc), sqlite3_errmsg(m_db), sql);
        sqlite3_finalize(stmt);
        return nullptr;
    }
    // 没有 header 的语句（PRAGMA 返回行失败前 / 非查询）也给列名。
    if (!header_done) {
        const int cols = sqlite3_column_count(stmt);
        for (int c = 0; c < cols; ++c) {
            const char* name = sqlite3_column_name(stmt, c);
            column_names.emplace_back(name ? name : "");
        }
    }
    sqlite3_finalize(stmt);
    return std::make_shared<SQLiteResult>(std::move(column_names), std::move(rows));
}

bool SQLiteConnection::execute(const std::string& sql) {
    return execute(sql, {});
}

bool SQLiteConnection::execute(const std::string& sql,
                               const std::vector<std::string>& params) {
    std::lock_guard<std::mutex> lock(m_mutex);
    // 裸事务 SQL 状态归一：调用方不走 begin_transaction/commit/rollback 而直接
    // execute("BEGIN/COMMIT/ROLLBACK/END") 时，m_inTransaction 必须保持真实，
    // 否则防御性单事务检查会误判。嵌套 BEGIN / 无事务 COMMIT 由 SQLite 本身
    // 报错（ok=false），状态不会被错误改写。接口方法自己管理标志、不经此处。
    const std::string kw = leading_keyword(sql);
    const bool raw_tx = kw == "BEGIN" || kw == "COMMIT" || kw == "ROLLBACK" || kw == "END";
    const bool ok = execute_locked(sql, params, params.empty() /* 无参数 → 允许多语句脚本 */);
    if (ok && raw_tx) m_inTransaction = (kw == "BEGIN");
    return ok;
}

bool SQLiteConnection::execute_locked(const std::string& sql,
                                      const std::vector<std::string>& params,
                                      bool allow_multi) {
    if (!m_connected || !m_db) {
        LOG_DATABASE_ERROR("SQLiteConnection::execute: not connected ({})", m_path);
        return false;
    }

    if (allow_multi && params.empty()) {
        // sqlite3_exec：initialize_script 一类多语句脚本走这里。
        char* err = nullptr;
        const int rc = sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, &err);
        if (rc != SQLITE_OK) {
            LOG_DB_QUERY_ERROR("SQLite exec error ({}: {}): {}",
                               sqlite_rc_name(rc), err ? err : "unknown", sql);
            sqlite3_free(err);
            return false;
        }
        return true;
    }

    // 带参数：prepare + bind + step（单语句；参数与语句的对应关系不允许多语句歧义）。
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(m_db, sql.c_str(), static_cast<int>(sql.size()), &stmt, nullptr);
    if (rc != SQLITE_OK) {
        LOG_DB_QUERY_ERROR("SQLite prepare error ({}: {}): {}",
                           sqlite_rc_name(rc), sqlite3_errmsg(m_db), sql);
        return false;
    }
    for (size_t i = 0; i < params.size(); ++i) {
        rc = sqlite3_bind_text(stmt, static_cast<int>(i + 1),
                               params[i].c_str(), static_cast<int>(params[i].size()),
                               SQLITE_TRANSIENT);
        if (rc != SQLITE_OK) {
            LOG_DB_QUERY_ERROR("SQLite bind error ({}: {}): {}",
                               sqlite_rc_name(rc), sqlite3_errmsg(m_db), sql);
            sqlite3_finalize(stmt);
            return false;
        }
    }
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        LOG_DB_QUERY_ERROR("SQLite step error ({}: {}): {}",
                           sqlite_rc_name(rc), sqlite3_errmsg(m_db), sql);
        return false;
    }
    return true;
}

bool SQLiteConnection::begin_transaction() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_inTransaction) {
        // 防御性拒绝：一条连接同一时刻只允许一个事务（路线图硬约束）。
        LOG_DATABASE_ERROR("SQLiteConnection::begin_transaction: connection '{}' already "
                           "in transaction — interleaved transactions are forbidden", m_path);
        return false;
    }
    // IMMEDIATE：BEGIN 即取写锁，把写锁升级失败的窗口消灭在 BEGIN 处；
    // busy_timeout 给出有界等待。DEFERRED 留给只读事务的显式配置。
    const bool ok = execute_locked(m_config.immediate_transactions ? "BEGIN IMMEDIATE;" : "BEGIN;",
                                   {}, true);
    if (ok) m_inTransaction = true;
    return ok;
}

bool SQLiteConnection::commit() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_inTransaction) {
        LOG_DATABASE_ERROR("SQLiteConnection::commit: no transaction active on '{}'", m_path);
        return false;
    }
    const bool ok = execute_locked("COMMIT;", {}, true);
    if (ok) m_inTransaction = false;
    return ok;   // COMMIT 失败（如 SQLITE_BUSY/IOERR）：事务仍开，调用方可重试或 rollback
}

bool SQLiteConnection::rollback() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_inTransaction) {
        LOG_DATABASE_ERROR("SQLiteConnection::rollback: no transaction active on '{}'", m_path);
        return false;
    }
    const bool ok = execute_locked("ROLLBACK;", {}, true);
    if (ok) m_inTransaction = false;   // 失败时事务可能仍开：保持标志，调用方可重试/断连兜底
    return ok;
}

long long SQLiteConnection::last_insert_rowid() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db) return 0;
    return sqlite3_last_insert_rowid(m_db);
}

// ====================================================================
// SQLiteService / 工厂
// ====================================================================

std::shared_ptr<IDBConnection> SQLiteService::create_connection(
    const std::string& host, const std::string& user, const std::string& password,
    const std::string& database, unsigned int port) {
    (void)host; (void)user; (void)password; (void)port;   // 本地文件库，仅 database 生效
    auto conn = std::make_shared<SQLiteConnection>(m_config);
    conn->set_connection_params(database);
    return conn;
}

std::shared_ptr<DBService> make_sqlite_service(SQLiteConfig config) {
    return std::make_shared<SQLiteService>(std::move(config));
}

std::shared_ptr<DBService> make_sqlite_async_service(
    std::shared_ptr<pr::ThreadPoolBase> workers,
    std::shared_ptr<pr::net::IExecutor> completion,
    SQLiteConfig config) {
    // 与 MariaDB 的 ba0d413 装饰同构：同步驱动不动，组合根显式包一层
    // Worker（跑同步 sqlite3 调用）+ 完成 executor（投递回调）边界。
    return std::make_shared<pr::AsyncDBServiceAdapter>(
        std::make_shared<SQLiteService>(std::move(config)),
        std::move(workers),
        [completion = std::move(completion)] { return completion; });
}

} // namespace mail_system
