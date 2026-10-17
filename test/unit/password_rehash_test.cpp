// rehash-on-login 回归测试（common/password_rehash.h）
//
// 守什么：MD5 / 明文历史行在验密成功后升级为 bcrypt 的链路——
//   1) 触发条件：只有非 bcrypt 才回写；bcrypt 行一次 DB 都不碰
//   2) CAS 参数：UPDATE 带 {new_hash, mail_address, old_stored}，缺旧值守卫 =
//      用户用旧密码登录期间管理员改的新密码会被 bcrypt(旧密码) 覆盖回去
//   3) 缓存刷新：成功后 AuthCache 条目变成新 bcrypt（明文不再躺在内存里一个 TTL），
//      user_id / shard 等其它字段原样保留
//   4) 失败不影响登录：UPDATE 失败 → done 仍恰好一次、缓存保持旧值（下次登录再试）
//   5) 新 hash 可验：bcrypt_verify(password, new_hash) 必须为 true
//   6) 无效连接：不崩，done 一次
//
// 用 MockDbConnection 的 Deferred 模式：先断言 UPDATE 已发起且参数正确、done 尚未
// 触发，再手动 fire 模拟 DB 回调，断言缓存与 done 计数。
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <memory>
#include <string>

#include "mail_system/back/common/password_rehash.h"
#include "mock_db_pool.h"

using namespace mail_system;
using mail_system::test::MockDbPool;

static int g_pass = 0, g_fail = 0;

static void check(const char* what, bool cond) {
    if (cond) { g_pass++; return; }
    g_fail++;
    std::printf("  FAIL %s\n", what);
}

int main() {
    const std::string user = "legacy@example.com";

    // 1+2+3+5：明文行 → CAS UPDATE（参数正确）→ fire 成功 → 缓存刷成可验的 bcrypt
    {
        auto pool = std::make_shared<MockDbPool>();
        auto mock = pool->mock_conn();
        mock->set_deferred(true);
        auto conn = pool->acquire_connection();
        auto cache = std::make_shared<AuthCache>();
        AuthCacheEntry entry{"plain-pw", 1, 42, 3};
        cache->store(user, entry);

        int done_calls = 0;
        rehash_password_if_needed_async(conn, cache, user, "plain-pw", "plain-pw", entry,
                                        [&] { ++done_calls; });

        check("plain: UPDATE pending before fire", mock->has_pending_execute());
        check("plain: done not yet called", done_calls == 0);
        check("plain: sql is CAS update",
              mock->last_query_sql().find("AND password = ?") != std::string::npos);

        auto params = mock->last_query_params();
        check("plain: 3 params", params.size() == 3);
        if (params.size() == 3) {
            check("plain: p0 is bcrypt",     is_bcrypt_hash(params[0]));
            check("plain: p0 verifies",      bcrypt_verify("plain-pw", params[0]));
            check("plain: p1 mail_address",  params[1] == user);
            check("plain: p2 old stored",    params[2] == "plain-pw");
        }

        // fire 之前缓存仍是明文（回写没落地不能提前刷）
        AuthCacheEntry before;
        check("plain: cache still legacy before fire",
              cache->lookup(user, before) && before.password_hash == "plain-pw");

        mock->fire_execute(true);
        check("plain: done exactly once", done_calls == 1);

        AuthCacheEntry got;
        check("plain: cache hit after upgrade",   cache->lookup(user, got));
        check("plain: cache upgraded to bcrypt",  is_bcrypt_hash(got.password_hash));
        check("plain: cache keeps user_id",       got.user_id == 42);
        check("plain: cache keeps shard",         got.shard == 3);
        check("plain: cache keeps status",        got.status == 1);
        check("plain: cache hash verifies",       verify_password_hash("plain-pw", got.password_hash));
        check("plain: cache hash rejects wrong",  !verify_password_hash("nope", got.password_hash));
    }

    // 1：bcrypt 行不触发任何 DB 操作，done 同步触发一次
    {
        auto pool = std::make_shared<MockDbPool>();
        auto mock = pool->mock_conn();
        mock->set_deferred(true);
        auto conn = pool->acquire_connection();
        auto cache = std::make_shared<AuthCache>();
        std::string bc = bcrypt_hash("x", 4);
        AuthCacheEntry entry{bc, 1, 1, 0};

        int done_calls = 0;
        rehash_password_if_needed_async(conn, cache, user, "x", bc, entry, [&] { ++done_calls; });

        check("bcrypt: no UPDATE issued",         !mock->has_pending_execute());
        check("bcrypt: done once synchronously",  done_calls == 1);
        check("bcrypt: sql untouched",            mock->last_query_sql().empty());
    }

    // 4：MD5 行 UPDATE 失败 → done 仍一次、缓存保持旧值
    {
        auto pool = std::make_shared<MockDbPool>();
        auto mock = pool->mock_conn();
        mock->set_deferred(true);
        auto conn = pool->acquire_connection();
        auto cache = std::make_shared<AuthCache>();
        const std::string md5_hello = "5d41402abc4b2a76b9719d911017c592";   // md5("hello")
        AuthCacheEntry entry{md5_hello, 1, 7, 1};
        cache->store(user, entry);

        int done_calls = 0;
        rehash_password_if_needed_async(conn, cache, user, "hello", md5_hello, entry,
                                        [&] { ++done_calls; });
        check("md5: UPDATE pending", mock->has_pending_execute());

        mock->fire_execute(false);
        check("md5 fail: done exactly once", done_calls == 1);

        AuthCacheEntry got;
        check("md5 fail: cache hit",          cache->lookup(user, got));
        check("md5 fail: cache keeps legacy", got.password_hash == md5_hello);
        check("md5 fail: legacy still verifies", verify_password_hash("hello", got.password_hash));
    }

    // 6：无效连接 → 不碰 DB、不崩、done 一次
    {
        auto cache = std::make_shared<AuthCache>();
        int done_calls = 0;
        rehash_password_if_needed_async(ScopedConnection::invalid(), cache, user, "pw", "pw",
                                        AuthCacheEntry{"pw", 1, 0, 0}, [&] { ++done_calls; });
        check("invalid conn: done exactly once", done_calls == 1);
    }

    // 空 cache 指针也不崩（无缓存部署）
    {
        auto pool = std::make_shared<MockDbPool>();
        auto mock = pool->mock_conn();
        mock->set_deferred(false);   // 同步模式：execute 立即回调
        auto conn = pool->acquire_connection();
        int done_calls = 0;
        rehash_password_if_needed_async(conn, nullptr, user, "pw", "pw",
                                        AuthCacheEntry{"pw", 1, 0, 0}, [&] { ++done_calls; });
        check("null cache: done exactly once", done_calls == 1);
        check("null cache: UPDATE was issued",
              mock->last_query_sql().find("UPDATE users SET password") != std::string::npos);
    }

    std::printf("password_rehash_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
