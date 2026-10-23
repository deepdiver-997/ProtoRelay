#ifndef MAIL_SYSTEM_SQLITE_POOL_H
#define MAIL_SYSTEM_SQLITE_POOL_H

#include "mail_system/back/db/db_pool.h"
#include <memory>
#include <mutex>

namespace mail_system {

// SQLite 连接池工厂：与 MariaDBPoolFactory 同构——池本体复用引擎无关的
// MySQLPool（只依赖 IDBConnection/DBService），只换 DBService 为 SQLiteService。
// `database` 配置字段在此方言下是库文件路径（AGENT_TMP_ROOT 内的进程独占文件）。
class SQLitePoolFactory : public DBPoolFactory {
public:
    ~SQLitePoolFactory() override = default;

    std::shared_ptr<DBPool> create_pool(
        const DBPoolConfig& config,
        std::shared_ptr<DBService> db_service
    ) override;

    static SQLitePoolFactory& get_instance();

private:
    SQLitePoolFactory() = default;
    static std::unique_ptr<SQLitePoolFactory> s_instance;
    static std::mutex s_mutex;

    SQLitePoolFactory(const SQLitePoolFactory&) = delete;
    SQLitePoolFactory& operator=(const SQLitePoolFactory&) = delete;
};

} // namespace mail_system

#endif // MAIL_SYSTEM_SQLITE_POOL_H
