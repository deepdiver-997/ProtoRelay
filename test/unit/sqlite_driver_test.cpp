// SQLite 驱动 v1 回归（protorelay_db_sqlite）。
//
// 覆盖面（设计依据：docs/local/2026-10-20-framework-decoupling-roadmap.md
// 「SQLite：可选的本地状态存储方向」）：
//   1. 同步驱动基本契约：连接/建表/参数绑定查询/事务/方言标识
//   2. 单连接单事务：BEGIN 期间拒绝再次 BEGIN；无事务拒绝 COMMIT/ROLLBACK
//   3. Worker/完成 executor 装饰（ba0d413 模式）：同步 sqlite3 调用在专用
//      DB Worker 上执行，回调投递到调用方指定的 IO executor；同连接并发第二
//      个 async 操作被立即拒绝（单飞行，杜绝事务交错）
//   4. 连接池集成：引擎无关 MySQLPool + SQLiteService，租约归还回基线
//   5. 有界 SQLITE_BUSY 等待：写锁被占时第二连接按 busy_timeout_ms 有界等待后
//      失败，锁释放后重试成功
//   6. 落盘强度显式可配：默认 WAL+FULL 生效；显式降级配置生效（驱动打 WARN，
//      不静默）
//
// 库文件：AGENT_TMP_ROOT/sqlite_driver_test_<pid>/（未设环境变量时退回系统临时
// 目录，同样进程独占），测试退出时自清理。无外部服务依赖。

#include "mail_system/back/db/sqlite_service.h"
#include "mail_system/back/db/sqlite_pool.h"
#include "mail_system/back/db/db_pool.h"
#include "framework/thread_pool/std_thread_pool.h"
#include "framework/thread_pool/io_thread_pool.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>   // getpid（进程独占临时目录命名）

using namespace mail_system;
namespace fs = std::filesystem;

namespace {

// 进程独占临时目录 + RAII 自清理。声明在 main 最前，析构晚于一切连接/池。
struct TempDirGuard {
    fs::path dir;
    explicit TempDirGuard(const char* label) {
        const char* root = std::getenv("AGENT_TMP_ROOT");
        fs::path base = root && *root ? fs::path(root)
                                      : fs::temp_directory_path() / "protorelay_sqlite_test";
        dir = base / (std::string(label) + "_" + std::to_string(::getpid()));
        fs::create_directories(dir);
    }
    ~TempDirGuard() {
        std::error_code ec;
        fs::remove_all(dir, ec);   // 清理失败不改变测试结论（目录进程独占）
    }
    fs::path file(const char* name) const { return dir / name; }
};

int g_failures = 0;
#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (cond) {                                                             \
            std::cout << "PASS: " << msg << "\n";                               \
        } else {                                                                \
            std::cerr << "FAIL: " << msg << "\n";                               \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

// ---------- 1. 同步驱动基本契约 ----------
void test_sync_basics(const fs::path& db_path) {
    // 直接构造具体类型：last_insert_rowid() 是 SQLite 方言的扩展入口，
    // 不在 IDBConnection 公共接口上（业务 SQL 不迁移的前提下保持接口最小）。
    auto conn = std::make_shared<SQLiteConnection>();
    conn->set_connection_params(db_path.string());
    CHECK(conn && conn->dialect() == IDBConnection::Dialect::SQLite,
          "dialect() reports SQLite");

    CHECK(conn->connect(), "connect opens local file db (creates on demand)");
    CHECK(conn->is_connected(), "is_connected true after connect");

    // 多语句脚本（无参数 execute 走 sqlite3_exec）
    CHECK(conn->execute(
              "CREATE TABLE IF NOT EXISTS items ("
              "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
              "  name TEXT NOT NULL,"
              "  qty  INTEGER NOT NULL DEFAULT 0);"
              "CREATE TABLE IF NOT EXISTS audit (note TEXT);"),
          "multi-statement DDL script executes");

    // 参数绑定 INSERT + rowid 回读
    CHECK(conn->execute("INSERT INTO items (name, qty) VALUES (?, ?);", {"alice", "3"}),
          "parameterized INSERT executes");
    CHECK(conn->last_insert_rowid() == 1, "last_insert_rowid returns first row id");
    CHECK(conn->execute("INSERT INTO items (name, qty) VALUES (?, ?);", {"bob", "5"}),
          "second parameterized INSERT executes");

    // 参数绑定 SELECT
    auto res = conn->query("SELECT id, name, qty FROM items WHERE name = ?;", {"bob"});
    CHECK(res && res->get_row_count() == 1, "parameterized SELECT returns one row");
    CHECK(res && res->get_value(0, "name") == "bob" && res->get_value(0, "qty") == "5",
          "bound parameter filters and columns map by name");

    // 错误 SQL → nullptr + last_error
    auto bad = conn->query("SELECT * FROM missing_table;");
    CHECK(!bad && !conn->get_last_error().empty(), "failed query returns null with last_error");

    // 空结果集是非错误
    auto empty = conn->query("SELECT id FROM items WHERE name = ?;", {"nobody"});
    CHECK(empty && empty->get_row_count() == 0, "no-match SELECT yields empty result, not null");

    conn->disconnect();
    CHECK(!conn->is_connected(), "disconnect closes the db handle");
}

// ---------- 2. 单连接单事务 ----------
void test_single_transaction_per_connection(const fs::path& db_path) {
    SQLiteConfig cfg;   // 默认：WAL + FULL + BEGIN IMMEDIATE
    auto conn = std::make_shared<SQLiteConnection>(cfg);
    conn->set_connection_params(db_path.string());
    CHECK(conn->connect(), "tx-test connection opens");

    CHECK(conn->execute("CREATE TABLE IF NOT EXISTS tx_t (v INTEGER);"),
          "tx table created");

    CHECK(conn->begin_transaction(), "first BEGIN IMMEDIATE succeeds");
    CHECK(!conn->begin_transaction(),
          "second BEGIN on same connection rejected (no interleaved transactions)");
    CHECK(conn->execute("INSERT INTO tx_t (v) VALUES (?);", {"1"}),
          "INSERT inside transaction executes");
    CHECK(conn->commit(), "COMMIT succeeds");
    CHECK(!conn->commit(), "COMMIT without active transaction rejected");
    CHECK(!conn->rollback(), "ROLLBACK without active transaction rejected");

    CHECK(conn->begin_transaction(), "second transaction BEGIN succeeds");
    conn->execute("INSERT INTO tx_t (v) VALUES (?);", {"2"});
    CHECK(conn->rollback(), "ROLLBACK succeeds");
    auto rows = conn->query("SELECT COUNT(*) AS n FROM tx_t;");
    CHECK(rows && rows->get_value(0, "n") == "1",
          "rolled-back INSERT left no row (1 committed row remains)");

    // 裸 SQL 事务的状态归一：绕过接口方法直接 execute("BEGIN...") 后，
    // commit() 必须能看到该事务（m_inTransaction 跟真实状态一致）
    CHECK(conn->execute("BEGIN IMMEDIATE;"), "raw SQL BEGIN executes");
    CHECK(conn->execute("INSERT INTO tx_t (v) VALUES (?);", {"3"}), "INSERT in raw-SQL tx");
    CHECK(conn->commit(), "commit() sees the raw-SQL transaction");

    conn->disconnect();
}

// ---------- 3. Worker / 完成 executor 装饰（ba0d413 模式） ----------
void test_worker_completion_decorator(const fs::path& db_path) {
    auto workers = std::make_shared<pr::StdThreadPool>(2);
    workers->start();
    auto io = std::make_shared<pr::IOThreadPool>(1);
    io->start();
    auto completion = io->next_executor();

    // 完成回调必须落在指定 IO executor 的线程上：先探出该线程 id
    std::promise<std::thread::id> probe_promise;
    auto probe_future = probe_promise.get_future();
    completion->post([&probe_promise] { probe_promise.set_value(std::this_thread::get_id()); });
    probe_future.wait();
    const std::thread::id completion_thread = probe_future.get();

    auto service = make_sqlite_async_service(workers, completion);
    CHECK(service->get_service_name() == "sqlite+worker", "decorated service reports +worker");

    // 装饰服务出的连接已是 AsyncDBConnectionAdapter：同步 sqlite3 调用跑在
    // workers 上，回调投递 completion（所属 executor）。
    auto conn = service->create_connection("", "", "", db_path.string(), 0);
    CHECK(conn && conn->dialect() == IDBConnection::Dialect::SQLite, "adapter forwards dialect");
    CHECK(conn->connect(), "adapter forwards connect");

    // CPS 链：CREATE → BEGIN IMMEDIATE(async_begin_transaction) → INSERT(绑定参数)
    // → COMMIT(async_commit)，全程持同一条连接（lease 贯穿完成链）；每步回调必须
    // 在指定完成 executor 的线程上执行（即 io 探针线程），否则视为契约破坏。
    // 事务走接口方法而非裸 SQL：驱动在连接上跟踪 m_inTransaction 并拒绝交叠事务。
    auto chain_done = std::make_shared<std::promise<bool>>();
    auto chain_future = chain_done->get_future();
    conn->async_execute("CREATE TABLE IF NOT EXISTS async_t (v TEXT);",
        [conn, chain_done, completion_thread](bool ok) {
            if (!ok || std::this_thread::get_id() != completion_thread) {
                chain_done->set_value(false); return;
            }
            conn->async_begin_transaction([conn, chain_done, completion_thread](bool ok) {
                if (!ok || std::this_thread::get_id() != completion_thread) {
                    chain_done->set_value(false); return;
                }
                conn->async_execute("INSERT INTO async_t (v) VALUES (?);",
                                    std::vector<std::string>{"from-worker"},
                                    [conn, chain_done, completion_thread](bool ok) {
                    if (!ok || std::this_thread::get_id() != completion_thread) {
                        chain_done->set_value(false); return;
                    }
                    conn->async_commit([chain_done](bool ok) { chain_done->set_value(ok); });
                });
            });
        });
    if (chain_future.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        std::cerr << "FAIL: async transaction chain did not complete within 5s\n";
        ++g_failures;
    } else {
        CHECK(chain_future.get() == true,
              "async BEGIN/INSERT/COMMIT chain completes on caller executor");
    }

    // 单飞行：busy 标志在调用线程同步置位，第二个并发 op 立即以失败完成
    auto first = std::make_shared<std::promise<bool>>();
    auto second = std::make_shared<std::promise<bool>>();
    conn->async_execute("INSERT INTO async_t (v) VALUES ('inflight');",
                        [first](bool ok) { first->set_value(ok); });
    conn->async_execute("INSERT INTO async_t (v) VALUES ('overlap');",
                        [second](bool ok) { second->set_value(ok); });   // 必被拒绝
    CHECK(second->get_future().get() == false,
          "concurrent second async op on same connection rejected (single-flight)");
    CHECK(first->get_future().get() == true, "first in-flight async op still completes");

    // 数据真落了盘（另一条独立连接读验证——WAL 下读写可并行）
    {
        auto check = std::make_shared<SQLiteConnection>();
        check->set_connection_params(db_path.string());
        check->connect();
        auto rows = check->query("SELECT COUNT(*) AS n FROM async_t WHERE v = 'from-worker';");
        CHECK(rows && rows->get_value(0, "n") == "1",
              "worker-committed async transaction is durable for a fresh reader");
        check->disconnect();
    }

    conn->disconnect();
    io->stop();
    workers->stop();
}

// ---------- 4. 连接池集成 ----------
void test_pool_integration(const fs::path& db_path) {
    DBPoolConfig cfg;
    cfg.database = db_path.string();   // SQLite 方言：database = 库文件路径
    cfg.initial_pool_size = 1;
    cfg.max_pool_size = 2;
    cfg.initialize_script.clear();     // 共享 MySQLPool 的脚本路径会注入 CREATE
                                       // DATABASE/USE（MySQL 方言专属），SQLite 走空脚本
    auto pool = SQLitePoolFactory::get_instance().create_pool(cfg, make_sqlite_service());
    CHECK(pool != nullptr, "SQLitePoolFactory creates engine-agnostic pool");
    CHECK(pool->get_pool_size() == 1, "pool honors initial_pool_size");

    const size_t baseline = pool->get_available_connections();
    {
        auto lease = pool->acquire_connection();
        CHECK(lease && lease->is_valid(), "pool checkout yields connected SQLite lease");
        CHECK(lease->operator->()->dialect() == IDBConnection::Dialect::SQLite,
              "pool connections report SQLite dialect");
        lease->operator->()->execute(
            "CREATE TABLE IF NOT EXISTS pool_t (id INTEGER PRIMARY KEY, v TEXT);");
        lease->operator->()->execute("INSERT INTO pool_t (v) VALUES (?);", {"pooled"});
    }
    CHECK(pool->get_available_connections() == baseline,
          "ScopedConnection RAII returns lease (available back to baseline)");

    auto rows = pool->acquire_connection()->operator->()->query("SELECT v FROM pool_t;");
    CHECK(rows && rows->get_row_count() == 1 && rows->get_value(0, "v") == "pooled",
          "data written via pooled connection is readable");
    pool->close();
}

// ---------- 5. 有界 SQLITE_BUSY 等待 ----------
void test_bounded_busy_wait(const fs::path& db_path) {
    SQLiteConfig holder_cfg;               // 默认 busy_timeout 5000ms
    auto holder = std::make_shared<SQLiteConnection>(holder_cfg);
    holder->set_connection_params(db_path.string());
    CHECK(holder->connect(), "lock holder connects");
    holder->execute("CREATE TABLE IF NOT EXISTS busy_t (v INTEGER);");
    CHECK(holder->begin_transaction(), "lock holder opens write transaction");
    holder->execute("INSERT INTO busy_t (v) VALUES (1);");

    SQLiteConfig contender_cfg;
    contender_cfg.busy_timeout_ms = 300;   // 有界等待：300ms 后必须放弃
    auto contender = std::make_shared<SQLiteConnection>(contender_cfg);
    contender->set_connection_params(db_path.string());
    CHECK(contender->connect(), "contender connects");

    const auto start = std::chrono::steady_clock::now();
    const bool wrote = contender->execute("INSERT INTO busy_t (v) VALUES (2);");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    CHECK(!wrote, "contending write fails while write lock is held");
    CHECK(elapsed.count() >= 200 && elapsed.count() < 2500,
          "contending write waited the bounded busy timeout (300ms) before failing ("
              + std::to_string(elapsed.count()) + "ms)");
    CHECK(contender->get_last_error().find("locked") != std::string::npos,
          "failure surfaces SQLITE_BUSY as database-is-locked error");

    CHECK(holder->commit(), "lock holder commits and releases the write lock");
    CHECK(contender->execute("INSERT INTO busy_t (v) VALUES (2);"),
          "contending write succeeds after lock release (retry is caller-visible)");
    auto rows = holder->query("SELECT COUNT(*) AS n FROM busy_t;");
    CHECK(rows && rows->get_value(0, "n") == "2", "both rows persisted");

    holder->disconnect();
    contender->disconnect();
}

// ---------- 6. 落盘强度显式可配 ----------
void test_durability_config(const fs::path& db_path) {
    {   // 默认：WAL + FULL
        auto conn = std::make_shared<SQLiteConnection>(SQLiteConfig{});
        conn->set_connection_params(db_path.string());
        CHECK(conn->connect(), "default-config connection opens");
        auto jm = conn->query("PRAGMA journal_mode;");
        CHECK(jm && jm->get_value(0, "journal_mode") == "wal",
              "default journal_mode is WAL (roadmap: WAL allows read/write parallelism)");
        auto syn = conn->query("PRAGMA synchronous;");
        CHECK(syn && syn->get_value(0, "synchronous") == "2",
              "default synchronous is FULL(2) — durability not silently relaxed");
        conn->disconnect();
    }
    {   // 显式降级：允许，但必须是配置写出来的（驱动侧 WARN，不静默）
        SQLiteConfig down;
        down.journal_mode = "DELETE";
        down.synchronous = "OFF";
        down.foreign_keys = false;
        auto conn = std::make_shared<SQLiteConnection>(down);
        conn->set_connection_params(db_path.string());
        CHECK(conn->connect(), "explicitly downgraded durability config still connects");
        auto jm = conn->query("PRAGMA journal_mode;");
        CHECK(jm && jm->get_value(0, "journal_mode") == "delete",
              "explicit journal_mode=DELETE takes effect");
        auto syn = conn->query("PRAGMA synchronous;");
        CHECK(syn && syn->get_value(0, "synchronous") == "0",
              "explicit synchronous=OFF takes effect (warned, never default)");
        conn->disconnect();
    }
    {   // foreign_keys：默认开
        auto conn = std::make_shared<SQLiteConnection>(SQLiteConfig{});
        conn->set_connection_params(db_path.string());
        conn->connect();
        conn->execute("CREATE TABLE IF NOT EXISTS fk_parent (id INTEGER PRIMARY KEY);"
                      "CREATE TABLE IF NOT EXISTS fk_child (pid INTEGER REFERENCES fk_parent(id));");
        const bool enforced = !conn->execute("INSERT INTO fk_child (pid) VALUES (99);");
        CHECK(enforced, "foreign_keys=ON rejects orphan child insert");
        conn->disconnect();
    }
}

} // namespace

int main() {
    TempDirGuard guard("sqlite_driver_test");   // 最先构造 → 最后析构清理

    // 各用例独立库文件，互不串数据
    test_sync_basics(guard.file("basics.db"));
    test_single_transaction_per_connection(guard.file("tx.db"));
    test_worker_completion_decorator(guard.file("async.db"));
    test_pool_integration(guard.file("pool.db"));
    test_bounded_busy_wait(guard.file("busy.db"));
    test_durability_config(guard.file("durability.db"));

    if (g_failures > 0) {
        std::cout << "FAIL: sqlite_driver_test had " << g_failures << " failing check(s)\n";
        return 1;
    }
    std::cout << "ALL PASS: sqlite_driver_test (sync + tx + worker/executor + pool + busy + durability)\n";
    return 0;
}
