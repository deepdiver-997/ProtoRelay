#ifndef MAIL_SYSTEM_DB_NULL_DB_POOL_H
#define MAIL_SYSTEM_DB_NULL_DB_POOL_H

#include "mail_system/back/db/db_pool.h"
#include <memory>

namespace mail_system {

// 空结果集 —— 始终返回 0 行
class NullDBResult : public IDBResult {
public:
    size_t get_row_count() const override { return 0; }
    size_t get_column_count() const override { return 0; }
    std::vector<std::string> get_column_names() const override { return {}; }
    std::map<std::string, std::string> get_row(size_t) const override { return {}; }
    std::vector<std::map<std::string, std::string>> get_all_rows() const override { return {}; }
    std::string get_value(size_t, const std::string&) const override { return ""; }
};

// 空数据库连接 —— 所有操作返回成功/空结果
class NullDBConnection : public IDBConnection {
public:
    bool connect() override { return true; }
    void disconnect() override {}
    bool is_connected() const override { return true; }
    std::shared_ptr<IDBResult> query(const std::string&) override {
        return std::make_shared<NullDBResult>();
    }
    std::shared_ptr<IDBResult> query(const std::string&, const std::vector<std::string>&) override {
        return std::make_shared<NullDBResult>();
    }
    bool execute(const std::string&) override { return true; }
    bool execute(const std::string&, const std::vector<std::string>&) override { return true; }
    bool begin_transaction() override { return true; }
    bool commit() override { return true; }
    bool rollback() override { return true; }
    std::string get_last_error() const override { return ""; }
    std::string escape_string(const std::string& str) const override { return str; }
};

// 空数据库连接池 —— 始终返回空连接，不连 MySQL
class NullDBPool : public DBPool {
public:
    NullDBPool() = default;

    size_t get_pool_size() const override { return 0; }
    size_t get_available_connections() const override { return 1; }
    size_t get_max_pool_size() const override { return 1; }
    size_t get_active_connections() const override { return 0; }
    void close() override {}

    std::shared_ptr<IDBConnection> get_connection() override {
        return std::make_shared<NullDBConnection>();
    }
    void release_connection(std::shared_ptr<IDBConnection>) override {}
    void initialize_pool() override {}
    std::shared_ptr<IDBConnection> create_connection() override {
        return std::make_shared<NullDBConnection>();
    }
};

} // namespace mail_system

#endif // MAIL_SYSTEM_DB_NULL_DB_POOL_H
