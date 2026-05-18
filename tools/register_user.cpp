// 用户注册工具
// 用法: ./register_user <db_config.json> <email> <password> <name> [phone]
//
// 密码通过 bcrypt 哈希后存入数据库，数据库中不存明文。

#include "mail_system/back/common/bcrypt.h"
#include "mail_system/back/db/db_pool.h"
#include "mail_system/back/db/db_service.h"
#include "mail_system/back/db/mysql_pool.h"
#include "mail_system/back/db/mysql_service.h"
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace mail_system;

int main(int argc, char* argv[]) {
    if (argc < 5) {
        std::cerr << "用法: " << argv[0]
                  << " <db_config.json> <email> <password> <name> [phone]\n\n"
                  << "示例:\n"
                  << "  " << argv[0]
                  << " config/db_config.json test@mail.hgmail.xin mypassword 'Test User'\n";
        return 1;
    }

    std::string db_config_path = argv[1];
    std::string email  = argv[2];
    std::string password = argv[3];
    std::string name   = argv[4];
    std::string phone  = (argc > 5) ? argv[5] : "";

    // 基本校验
    if (email.empty() || password.empty() || name.empty()) {
        std::cerr << "错误: email/password/name 不能为空\n";
        return 1;
    }

    // 加载数据库配置
    DBPoolConfig db_config;
    if (!db_config.loadFromJson(db_config_path)) {
        std::cerr << "错误: 无法加载数据库配置文件 " << db_config_path << "\n";
        return 1;
    }

    // 创建数据库连接池
    auto db_pool = MySQLPoolFactory::get_instance().create_pool(
        db_config, std::make_shared<MySQLService>());

    // 检查用户是否已存在
    {
        auto conn = db_pool->get_connection();
        if (!conn || !conn->is_connected()) {
            std::cerr << "错误: 无法连接数据库\n";
            return 1;
        }
        auto result = conn->query("SELECT COUNT(*) as cnt FROM users WHERE mail_address = ?",
                                  {email});
        if (result && result->get_row_count() > 0 &&
            std::stoul(result->get_value(0, "cnt")) > 0) {
            std::cerr << "错误: 用户 " << email << " 已存在\n";
            return 1;
        }
    }

    // bcrypt 哈希密码
    std::string hashed = bcrypt_hash(password);
    if (hashed.empty()) {
        std::cerr << "错误: bcrypt 哈希失败（可能需要 OpenSSL RAND_bytes 支持）\n";
        return 1;
    }

    // 插入用户
    {
        auto conn = db_pool->get_connection();
        if (!conn || !conn->is_connected()) {
            std::cerr << "错误: 无法连接数据库\n";
            return 1;
        }

        std::string sql;
        if (phone.empty()) {
            sql = "INSERT INTO users (mail_address, password, name) VALUES (?, ?, ?)";
            if (!conn->execute(sql, {email, hashed, name})) {
                std::cerr << "错误: 插入用户失败\n";
                return 1;
            }
        } else {
            sql = "INSERT INTO users (mail_address, password, name, telephone) "
                  "VALUES (?, ?, ?, ?)";
            if (!conn->execute(sql, {email, hashed, name, phone})) {
                std::cerr << "错误: 插入用户失败\n";
                return 1;
            }
        }
    }

    std::cout << "用户注册成功!\n"
              << "  邮箱:    " << email << "\n"
              << "  姓名:    " << name << "\n"
              << "  bcrypt:  " << hashed.substr(0, 7) << "...\n"
              << "  (触发器已自动创建默认邮箱: 收件箱/发件箱/垃圾箱/已删除/草稿箱)\n";
    return 0;
}
