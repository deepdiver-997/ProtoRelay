// MariaDB 非阻塞 async 的回归测试（阶段 2）。
//
// 剧情：连接池的 available 连接数在跑完一批 async 查询后必须回到基线。
// 这直接守卫 2026-08-29 修掉的一个真 bug：AsyncStmtOp::done 若捕获 op 自身，
// op↔done 构成 shared_ptr 循环 → op（连同捕获了 ScopedConnection 的用户回调）
// 永远不释放 → 每个 async 查询泄漏一条连接 → 池耗尽 → io 线程卡 5s
// connection_timeout。压测复现：89 次 acquire 只有 1 次 release。
//
// 剧情 2（2026-09-05）：结果列超出 256 字节绑定缓冲时，mysql_stmt_fetch 返回
// MYSQL_DATA_TRUNCATED(101)（行可用、列被截断），旧代码视作错误中止整个结果集
// → 生产事故：test3 收件箱一封 285 字节主题邮件让 IMAP UID SEARCH 整体失败
// （NO Server error），客户端表现即"账号连不上"。修复后必须完整取回长列。
//
// 依赖真实 DB（config/db_config.json 指向的 MySQL/MariaDB 服务 + mail 库）：
// 文件不存在或连不上 → 静默 SKIP（返回 0），不影响 CI。
// 本测试跑在主线程（无 io_context 注册）→ async 走阻塞 poll 路径，回调同步触发。

#include "mail_system/back/db/mariadb_pool.h"
#include "mail_system/back/db/mariadb_service.h"
#include "mail_system/back/db/mysql_service.h"
#include "mail_system/back/db/db_pool.h"
#include <iostream>

using namespace mail_system;

namespace {

// >256 字节 ASCII 值，踩中旧的 256 字节默认缓冲。刻意用 ASCII：
// UTF-8 字面量会被非 utf8 连接字符集 reinterpret（实测 900 字节
// 长度对内容错），与被测逻辑无关的假阳性。
const char* kBigExpr = "REPEAT('x', 900)";

} // namespace

int main() {
    DBPoolConfig cfg;
    bool loaded = false;
    // 测试从 build/ 目录跑（ctest WorkingDirectory），config 在项目根
    for (const char* p : {"config/db_config.json", "../config/db_config.json"}) {
        if (cfg.loadFromJson(p)) { loaded = true; break; }
    }
    if (!loaded) {
        std::cout << "SKIP: db_config.json not found\n";
        return 0;
    }

    auto pool = MariaDBPoolFactory::get_instance().create_pool(
        cfg, std::make_shared<MariaDBService>());
    if (!pool) { std::cout << "SKIP: pool creation failed\n"; return 0; }

    // 探活：连不上真实 DB 就 skip（本地 CI 无 DB 也能过）
    {
        auto probe = pool->acquire_connection();
        if (!probe->is_valid()) {
            std::cout << "SKIP: database unavailable (" << pool->get_available_connections()
                      << " avail)\n";
            return 0;
        }
    }

    const size_t initial_avail = pool->get_available_connections();
    int completed = 0;
    constexpr int kQueries = 50;

    for (int i = 0; i < kQueries; ++i) {
        auto sc = pool->acquire_connection();
        if (!sc->is_valid()) break;
        bool done = false;
        sc->operator->()->async_query("SELECT 1", [&](std::shared_ptr<IDBResult> r) { done = (r != nullptr); });
        // 主线程 → 阻塞 poll 路径，回调同步触发；失败即代表 async 链断了
        if (done) ++completed;
        // sc 出作用域 → release_connection。旧代码（done 捕获 op 的循环）这里不归还，
        // available 持续下降，最后一条断言触发 FAIL。
    }

    const size_t after_avail = pool->get_available_connections();

    if (completed != kQueries) {
        std::cout << "FAIL: only " << completed << "/" << kQueries
                  << " async queries completed\n";
        return 1;
    }
    if (after_avail < initial_avail) {
        std::cout << "FAIL: connection leak — available dropped from " << initial_avail
                  << " to " << after_avail << " after " << kQueries << " async queries\n";
        return 1;
    }

    std::cout << "PASS: " << completed << " async queries, pool available "
              << initial_avail << " -> " << after_avail << " (no leak)\n";

    // ---- 剧情 2：>256 字节列必须完整取回（MYSQL_DATA_TRUNCATED 不得中止结果集）----
    {
        std::string want(900, 'x');

        // 1) MariaDB async 路径（AsyncStmtOp 结果拉取）
        {
            auto sc = pool->acquire_connection();
            if (!sc->is_valid()) { std::cout << "FAIL: acquire for big-column test\n"; return 1; }
            std::shared_ptr<IDBResult> res;
            bool done = false;
            sc->operator->()->async_query(
                std::string("SELECT ") + kBigExpr + " AS big_subject",
                [&](std::shared_ptr<IDBResult> r) { res = r; done = true; });
            if (!done || !res) {
                std::cout << "FAIL: async big-column query returned null result\n";
                return 1;
            }
            if (res->get_row_count() != 1 || res->get_value(0, "big_subject") != want) {
                const std::string got = res->get_value(0, "big_subject");
                size_t xc = 0; unsigned char first_nonx = 0;
                for (char c : got) {
                    if (c == 'x') ++xc;
                    else if (!first_nonx) first_nonx = (unsigned char)c;
                }
                std::printf("FAIL: async big-column wrong (len=%zu expect=%zu xcount=%zu "
                            "first_nonx=%02x hex[0..15]:", got.size(), want.size(), xc, first_nonx);
                for (int k = 0; k < 16 && k < (int)got.size(); ++k)
                    std::printf(" %02x", (unsigned char)got[k]);
                std::printf("\n");
                return 1;
            }
        }

        // 2) MySQL 同步 query 路径（生产 IMAP/SMTP 的 MySQLConnection::query）
        {
            MySQLService svc;
            auto conn = svc.create_connection(cfg.host, cfg.user, cfg.password,
                                              cfg.database, cfg.port);
            auto res = conn->query(std::string("SELECT ") + kBigExpr + " AS big_subject", {});
            if (!res) {
                std::cout << "FAIL: mysql sync big-column query returned null result\n";
                return 1;
            }
            if (res->get_row_count() != 1 || res->get_value(0, "big_subject") != want) {
                std::cout << "FAIL: mysql sync big-column value wrong/truncated (len="
                          << (res->get_row_count() ? res->get_value(0, "big_subject").size() : 0)
                          << ", expect " << want.size() << ")\n";
                return 1;
            }
        }

        std::cout << "PASS: 900-byte column fetched intact via async + sync (DATA_TRUNCATED handled)\n";
    }

    // ---- 剧情 3（2026-09-06）：截断行后面还有行 —— UAF 回归 ----
    // d3a739c 的截断重取在 buffers[i].resize() 释放旧 256 字节缓冲后没有
    // 重绑 stmt 内部的 bind（bind_result 是按值拷贝），下一行 mysql_stmt_fetch
    // 会把数据 memcpy 进已释放的旧缓冲（ASan: WRITE of size 256 → 生产
    // tcache 连环崩溃）。**必须多行且长列在靠前的行**：剧情 2 的单行长列
    // 没有"下一行"，踩不到悬垂指针，因此拦不住该回归。ASan 单测构建下
    // 本场景在修复缺失时直接 abort。
    {
        const std::string sql =
            "SELECT REPEAT('x', 900) AS v, 1 AS ord "
            "UNION ALL SELECT 'short-1', 2 "
            "UNION ALL SELECT 'short-2', 3 "
            "ORDER BY ord";
        std::string want_long(900, 'x');

        // 1) MariaDB async 路径
        {
            auto sc = pool->acquire_connection();
            if (!sc->is_valid()) { std::cout << "FAIL: acquire for multi-row test\n"; return 1; }
            std::shared_ptr<IDBResult> res;
            bool done = false;
            sc->operator->()->async_query(sql,
                [&](std::shared_ptr<IDBResult> r) { res = r; done = true; });
            if (!done || !res) {
                std::cout << "FAIL: async multi-row query returned null result\n";
                return 1;
            }
            if (res->get_row_count() != 3
                || res->get_value(0, "v") != want_long
                || res->get_value(1, "v") != "short-1"
                || res->get_value(2, "v") != "short-2") {
                std::cout << "FAIL: async multi-row values corrupted (UAF on stale bind?)\n";
                return 1;
            }
        }

        // 2) MySQL 同步 query 路径（生产 IMAP/SMTP 的 MySQLConnection::query）
        {
            MySQLService svc;
            auto conn = svc.create_connection(cfg.host, cfg.user, cfg.password,
                                              cfg.database, cfg.port);
            auto res = conn->query(sql, {});
            if (!res) {
                std::cout << "FAIL: mysql sync multi-row query returned null result\n";
                return 1;
            }
            if (res->get_row_count() != 3
                || res->get_value(0, "v") != want_long
                || res->get_value(1, "v") != "short-1"
                || res->get_value(2, "v") != "short-2") {
                std::cout << "FAIL: mysql sync multi-row values corrupted (UAF on stale bind?)\n";
                return 1;
            }
        }

        std::cout << "PASS: truncated long column in multi-row result, later rows intact\n";
    }

    return 0;
}
