#ifndef MAIL_SYSTEM_DB_POOL_H
#define MAIL_SYSTEM_DB_POOL_H

#include "framework/db/db_pool.h"
#include "framework/db/db_pool_config.h"
#include "mail_system/back/db/db_service.h"
#include "mail_system/back/common/logger.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <string>
#include <memory>
#include <vector>

namespace mail_system {

// 向后兼容
using pr::DBPool;
using pr::ScopedConnection;
using pr::IDBConnection;
using pr::IDBResult;
using pr::QueryCallback;
using pr::ExecuteCallback;
// DBPoolConfig 已上移 framework/db/db_pool_config.h（2026-10-17 include 泄漏清理），
// 此处保留别名，应用代码无需改动。
using pr::DBPoolConfig;

// ================================================================
// DBPoolFactory — 连接池工厂 (应用层, 具体实现由 app 提供)
// ================================================================
class DBPoolFactory {
public:
    virtual ~DBPoolFactory() = default;
    virtual std::shared_ptr<DBPool> create_pool(
        const DBPoolConfig& config, std::shared_ptr<DBService> db_service) = 0;
protected:
    DBPoolFactory() = default;
};

} // namespace mail_system
#endif // MAIL_SYSTEM_DB_POOL_H
