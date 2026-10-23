#ifndef MAIL_SYSTEM_SQLITE_SERVICE_H
#define MAIL_SYSTEM_SQLITE_SERVICE_H

#include "mail_system/back/db/db_service.h"
#include "framework/net/executor.h"
#include "framework/thread_pool/thread_pool_base.h"
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;   // 全局前置声明（实现文件才 include <sqlite3.h>，头文件不引 SDK）

namespace mail_system {

// ====================================================================
// SQLiteConfig —— 落盘强度与并发行为显式配置
// ====================================================================
// 设计依据：framework-decoupling-roadmap「SQLite：可选的本地状态存储方向」。
// 默认值取**最持久**组合（WAL + FULL），任何放弃持久性的取值必须显式写出，
// 驱动在 open 时对降级配置打 WARN 日志——绝不静默放宽落盘强度。
struct SQLiteConfig {
    // journal_mode PRAGMA（"WAL" / "DELETE" / "TRUNCATE" / "PERSIST" / "MEMORY" / "OFF"）。
    // 空串 = 不下发该 PRAGMA（沿用库文件既有模式）。MEMORY/OFF 视为放弃持久性 → WARN。
    std::string journal_mode = "WAL";
    // synchronous PRAGMA（"FULL" / "NORMAL" / "OFF"）。空串 = 不下发。OFF → WARN。
    std::string synchronous = "FULL";
    // 有界 SQLITE_BUSY 等待：sqlite3_busy_timeout 毫秒数。0 = 立即失败不等待。
    int busy_timeout_ms = 5000;
    bool foreign_keys = true;
    // BEGIN 语义：true（默认）= BEGIN IMMEDIATE，开启事务即取写锁，
    // 把"事务中途升级被 SQLITE_BUSY 拒绝"的窗口消灭在 BEGIN 处；
    // false = 普通 DEFERRED BEGIN。两种取值都配合 busy_timeout 生效。
    bool immediate_transactions = true;
};

// ====================================================================
// SQLiteResult —— 查询结果（IDBResult 实现，行/列全量物化为文本）
// ====================================================================
// sqlite3_column_text 统一取值：SQLite 是动态类型，文本化与
// MySQL/MariaDB 驱动的 string 结果语义对齐，调用方零分支。
class SQLiteResult : public IDBResult {
public:
    SQLiteResult(std::vector<std::string> column_names,
                 std::vector<std::vector<std::string>> rows);

    size_t get_row_count() const override;
    size_t get_column_count() const override;
    std::vector<std::string> get_column_names() const override;
    std::map<std::string, std::string> get_row(size_t row_index) const override;
    std::vector<std::map<std::string, std::string>> get_all_rows() const override;
    std::string get_value(size_t row_index, const std::string& column_name) const override;

private:
    std::vector<std::string> m_columnNames;
    std::vector<std::vector<std::string>> m_rows;
};

// ====================================================================
// SQLiteConnection —— IDBConnection 实现（同步 sqlite3 C API）
// ====================================================================
// 连接模型（路线图定版）：
//   * 一条连接 = 一个 sqlite3 句柄，指向一个本地文件（`database` 参数即文件路径，
//     支持 ":memory:"）；host/user/password/port 对本地库无意义，忽略。
//   * 单飞行（single-flight）：m_mutex 把一条连接上的全部操作串行化——
//     同一时刻一个连接只跑一个操作；in_transaction_ 标志防御性地拒绝在已开
//     事务的连接上再 BEGIN、在无事务时 COMMIT/ROLLBACK，结构上杜绝同一连接
//     交错两个事务。事务与连接的生命周期绑定由调用方（池的 ScopedConnection
//     租约 / async 完成链捕获 lease）保证贯穿。
//   * async_* 不在这里 override：v1 的 Worker/完成 executor 边界由框架的
//     AsyncDBConnectionAdapter/AsyncDBServiceAdapter（ba0d413 模式）装饰提供，
//     见 make_sqlite_async_service()。
class SQLiteConnection : public IDBConnection {
public:
    SQLiteConnection() = default;
    explicit SQLiteConnection(SQLiteConfig config);
    ~SQLiteConnection() override;

    // DBService 工厂入参映射：database = 库文件路径，其余参数忽略。
    void set_connection_params(const std::string& database);

    bool connect() override;     // sqlite3_open + PRAGMA 配置（WAL/synchronous/busy_timeout/foreign_keys）
    void disconnect() override;  // sqlite3_close；未提交事务被 SQLite 回滚
    Dialect dialect() const override { return Dialect::SQLite; }
    bool is_connected() const override;

    std::shared_ptr<IDBResult> query(const std::string& sql) override;
    std::shared_ptr<IDBResult> query(const std::string& sql,
                                     const std::vector<std::string>& params) override;
    bool execute(const std::string& sql) override;   // sqlite3_exec：允许多语句脚本
    bool execute(const std::string& sql,
                 const std::vector<std::string>& params) override;  // 单语句 + ? 绑定
    bool begin_transaction() override;               // 默认 BEGIN IMMEDIATE
    bool commit() override;
    bool rollback() override;
    std::string get_last_error() const override;     // sqlite3_errmsg（线程局部文本）
    std::string escape_string(const std::string& str) const override;  // ' → ''（SQLite 无反斜杠转义）

    // 供 async 装饰层（AsyncDBConnectionAdapter::async_in_flight 聚合）之外的
    // 直接检测：sync 驱动默认无在飞操作，保持基类 false 即可。

    // LAST_INSERT_ID() 的 SQLite 对应物：业务 SQL 不迁移（v1 范围红线），
    // 但轻量节点 schema 会需要自增主键回读，提供显式入口。
    long long last_insert_rowid() const;

private:
    std::shared_ptr<IDBResult> query_locked(const std::string& sql,
                                            const std::vector<std::string>& params);
    bool execute_locked(const std::string& sql, const std::vector<std::string>& params,
                        bool allow_multi);

    SQLiteConfig m_config;
    sqlite3* m_db = nullptr;          // ::sqlite3 句柄（头文件不引 <sqlite3.h>）
    bool m_connected = false;
    bool m_inTransaction = false;
    mutable std::mutex m_mutex;
    std::string m_path;               // open 时的库文件路径（错误信息用）
};

// ====================================================================
// SQLiteService —— DBService 工厂
// ====================================================================
class SQLiteService : public DBService {
public:
    explicit SQLiteService(SQLiteConfig config = {}) : m_config(std::move(config)) {}
    ~SQLiteService() override = default;

    std::shared_ptr<IDBConnection> create_connection(
        const std::string& host, const std::string& user, const std::string& password,
        const std::string& database, unsigned int port) override;   // 仅 database（文件路径）生效

    std::string get_service_name() const override { return "sqlite"; }

private:
    SQLiteConfig m_config;
};

// 组合根便捷入口：等价 std::make_shared<SQLiteService>(config)。
std::shared_ptr<DBService> make_sqlite_service(SQLiteConfig config = {});

// ====================================================================
// Worker / 完成 executor 装饰（ba0d413 模式，与 MariaDB 驱动同款）
// ====================================================================
// 少量专用 DB Worker（ThreadPoolBase）执行同步 sqlite3 API，结果回调投递到
// 调用方指定的完成 executor（IExecutor，通常是节点 runtime 的 IO executor），
// 绝不阻塞网络 IO 线程。单飞行语义由 AsyncDBConnectionAdapter 的 busy 标志
// 提供：同一条连接上第二个并发 async 操作立即以失败完成回调（不排队、不阻塞）。
//   * workers    ：DB 专用 Worker 池（建议少量线程，SQLite 本地写是串行资源）
//   * completion ：完成回调归属的 executor；必须保持运行直到全部回调跑完
//   * config     ：SQLiteConfig，透传给底层 SQLiteService
std::shared_ptr<DBService> make_sqlite_async_service(
    std::shared_ptr<pr::ThreadPoolBase> workers,
    std::shared_ptr<pr::net::IExecutor> completion,
    SQLiteConfig config = {});

} // namespace mail_system

#endif // MAIL_SYSTEM_SQLITE_SERVICE_H
