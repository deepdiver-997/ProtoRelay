#include "mail_system/back/db/sqlite_pool.h"
#include "mail_system/back/db/mysql_pool.h"
#include "mail_system/back/db/sqlite_service.h"

namespace mail_system {

std::unique_ptr<SQLitePoolFactory> SQLitePoolFactory::s_instance = nullptr;
std::mutex SQLitePoolFactory::s_mutex;

std::shared_ptr<DBPool> SQLitePoolFactory::create_pool(
    const DBPoolConfig& config, std::shared_ptr<DBService> db_service) {
    // 池引擎无关：MySQLPool 只依赖 IDBConnection/DBService。未显式给服务时
    // 兜底 SQLiteService（默认 WAL+FULL 落盘强度）。
    if (!db_service) {
        db_service = std::make_shared<SQLiteService>();
    }
    return std::make_shared<MySQLPool>(config, std::move(db_service));
}

SQLitePoolFactory& SQLitePoolFactory::get_instance() {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_instance) {
        s_instance = std::unique_ptr<SQLitePoolFactory>(new SQLitePoolFactory());
    }
    return *s_instance;
}

} // namespace mail_system
