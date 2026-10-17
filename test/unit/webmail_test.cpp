// webmail_test — webmail MVP 单测（无 DB、无网络）
//   SessionStore：token 生成/登记/查询/注销/TTL 过期；cookie 解析
//   AuthService：缓存命中路径（预注入 AuthCache，无需 DB）+ 三态结果
//   ApiRouter：路由表派发（认领语义 / 405 / 404 / 登录全流程 / 会话自省）
//   MailService/mail_api：MockDbPool 延迟回调驱动三段查询链
//     （归属校验→count→分页；参数序/钳制/404/401/503/400 全覆盖）
#undef NDEBUG
#include <cassert>
#include <nlohmann/json.hpp>

#include "framework/http/http_types.hpp"
#include "mail_system/back/webmail/api/api_router.h"
#include "mail_system/back/webmail/auth_service.h"
#include "mail_system/back/webmail/mail_service.h"
#include "mail_system/back/webmail/session_store.h"
#include "mail_system/back/common/bcrypt.h"
#include "mail_system/back/router/static_shard_router.h"
#include "mock_db_pool.h"

#include <iostream>
#include <optional>
#include <string>

using namespace mail_system;
using namespace mail_system::webmail;
namespace http = pr::http;

// ========== 夹具 ==========

static http::HttpRequest make_req(std::string method, std::string path,
                                  std::string body = {}, std::string cookie = {}) {
    http::HttpRequest r;
    r.method = std::move(method);
    // 与真实 parser 对齐：'?' 前是 path，之后是 query
    size_t qmark = path.find('?');
    if (qmark != std::string::npos) {
        r.query = path.substr(qmark + 1);
        path.resize(qmark);
    }
    r.path = std::move(path);
    r.body = std::move(body);
    if (!cookie.empty()) r.headers["cookie"] = cookie;
    return r;
}

struct Captured {
    int calls = 0;
    std::optional<http::HttpResponse> resp;
    api::Complete complete() {
        return [this](http::HttpResponse r) {
            ++calls;
            resp = std::move(r);
        };
    }
};

// 预注入认证缓存：user@test.local（bcrypt("secret")，user_id=42）、
// disabled@test.local（status=0 禁用负缓存）
struct ApiFixture {
    std::shared_ptr<AuthCache> cache = std::make_shared<AuthCache>();
    SessionStore sessions{1000, std::chrono::seconds(60)};
    std::unique_ptr<AuthService> auth;
    std::unique_ptr<ApiRouter> router;

    ApiFixture() {
        AuthCacheEntry active;
        active.password_hash = bcrypt_hash("secret");
        active.status = 1;
        active.user_id = 42;
        cache->inject("user@test.local", active);

        AuthCacheEntry disabled;
        disabled.password_hash = "";
        disabled.status = 0;
        cache->inject("disabled@test.local", disabled);

        // router=nullptr：缓存命中路径不触 DB，足够覆盖编排逻辑；
        // 缓存 miss 会走 503（另有用例固定该 fail-closed 语义）
        auth = std::make_unique<AuthService>(nullptr, cache, nullptr);
        mail = std::make_unique<MailService>(nullptr);   // 邮件端点在本夹具 → 503
        router = std::make_unique<ApiRouter>(*auth, sessions, *mail);
    }

    std::unique_ptr<MailService> mail;
};

// ========== SessionStore ==========

static void test_session_store_basics() {
    SessionStore store;
    std::string t1 = SessionStore::generate_token();
    std::string t2 = SessionStore::generate_token();
    assert(t1.size() == 64);
    assert(t2.size() == 64 && t1 != t2);          // 256 位随机，不重复
    for (char c : t1) assert(isxdigit(static_cast<unsigned char>(c)));

    store.put(t1, 7, "a@test.local");
    WebSessionInfo info;
    assert(store.lookup(t1, info));
    assert(info.user_id == 7 && info.address == "a@test.local");

    assert(!store.lookup("nope", info));          // 未登记 token
    assert(!store.lookup("", info));              // 空 token 一律拒绝

    store.erase(t1);
    assert(!store.lookup(t1, info));              // 注销即失效
    std::cout << "  [PASS] session_store_basics" << std::endl;
}

static void test_session_store_ttl_expiry() {
    SessionStore store{100, std::chrono::seconds(0)};   // TTL=0 → 立即过期
    std::string t = SessionStore::generate_token();
    store.put(t, 1, "x@test.local");
    WebSessionInfo info;
    assert(!store.lookup(t, info));               // stale 视为未登录
    std::cout << "  [PASS] session_store_ttl_expiry" << std::endl;
}

static void test_cookie_parse() {
    http::HttpRequest req;
    // 无 cookie 头
    assert(SessionStore::token_from_request(req).empty());
    // 单 cookie
    req.headers["cookie"] = "wb_session=abc123";
    assert(SessionStore::token_from_request(req) == "abc123");
    // 多 cookie（本会话不在第一位）
    req.headers["cookie"] = "theme=dark; wb_session=tok42; lang=zh";
    assert(SessionStore::token_from_request(req) == "tok42");
    // 前缀陷阱：xwb_session 不能误命中
    req.headers["cookie"] = "xwb_session=evil; wb_session=good";
    assert(SessionStore::token_from_request(req) == "good");
    // 无本会话 cookie
    req.headers["cookie"] = "theme=dark";
    assert(SessionStore::token_from_request(req).empty());
    std::cout << "  [PASS] cookie_parse" << std::endl;
}

// ========== AuthService（缓存命中路径）==========

static void test_auth_service_outcomes() {
    ApiFixture fx;
    AuthOutcome out;

    fx.auth->login_async("user@test.local", "secret", [&](AuthOutcome o) { out = o; });
    assert(out.ok && out.user_id == 42 && !out.server_error);

    out = {};
    fx.auth->login_async("user@test.local", "wrong", [&](AuthOutcome o) { out = o; });
    assert(!out.ok && !out.server_error);         // 密码错 → 401 语义

    out = {};
    fx.auth->login_async("disabled@test.local", "whatever", [&](AuthOutcome o) { out = o; });
    assert(!out.ok && !out.server_error);         // 禁用 → 同口径 401

    out = {};
    fx.auth->login_async("ghost@test.local", "x", [&](AuthOutcome o) { out = o; });
    assert(!out.ok && out.server_error);          // 缓存 miss + 无 DB → 503 fail-closed
    std::cout << "  [PASS] auth_service_outcomes" << std::endl;
}

// ========== ApiRouter 路由表 ==========

static void test_router_claim_semantics() {
    ApiFixture fx;
    Captured c;

    // 非 /api 前缀 → 不认领（落静态文件管线）
    assert(!fx.router->handle(make_req("GET", "/index.html"), c.complete()));
    assert(!fx.router->handle(make_req("GET", "/api"), c.complete()));
    assert(c.calls == 0);

    // health：GET 认领 + 200 JSON
    assert(fx.router->handle(make_req("GET", "/api/health"), c.complete()));
    assert(c.calls == 1 && c.resp->status == 200);
    assert(c.resp->content_type == "application/json");
    assert(c.resp->body.find("\"ok\"") != std::string::npos ||
           c.resp->body.find("ok") != std::string::npos);

    // health：POST → 405
    c = {};
    assert(fx.router->handle(make_req("POST", "/api/health"), c.complete()));
    assert(c.resp->status == 405);

    // 未知 API → 认领 + 404（API 404 不落静态，避免 /api/* 泄漏目录列表）
    c = {};
    assert(fx.router->handle(make_req("GET", "/api/nonexistent"), c.complete()));
    assert(c.resp->status == 404);
    std::cout << "  [PASS] router_claim_semantics" << std::endl;
}

static void test_router_login_flow() {
    ApiFixture fx;
    Captured c;

    // 畸形 body → 400
    assert(fx.router->handle(
        make_req("POST", "/api/login", "not json"), c.complete()));
    assert(c.resp->status == 400);
    c = {};
    assert(fx.router->handle(
        make_req("POST", "/api/login", R"({"address":"user@test.local"})"), c.complete()));
    assert(c.resp->status == 400);

    // 错误密码 → 401
    c = {};
    assert(fx.router->handle(
        make_req("POST", "/api/login", R"({"address":"user@test.local","password":"wrong"})"),
        c.complete()));
    assert(c.resp->status == 401);
    assert(c.calls == 1);

    // 正确密码 → 200 + Set-Cookie + user 信息
    c = {};
    assert(fx.router->handle(
        make_req("POST", "/api/login", R"({"address":"user@test.local","password":"secret"})"),
        c.complete()));
    assert(c.resp->status == 200 && c.calls == 1);
    assert(c.resp->extra_headers.find("Set-Cookie: wb_session=") != std::string::npos);
    assert(c.resp->extra_headers.find("HttpOnly") != std::string::npos);
    assert(c.resp->extra_headers.find("SameSite=Lax") != std::string::npos);
    assert(c.resp->body.find("\"id\":42") != std::string::npos);

    // 提取 cookie → /api/me 自省 200
    std::string set_cookie = c.resp->extra_headers;
    size_t eq = set_cookie.find("wb_session=") + strlen("wb_session=");
    size_t semi = set_cookie.find(';', eq);
    std::string token = set_cookie.substr(eq, semi - eq);
    assert(token.size() == 64);

    c = {};
    assert(fx.router->handle(make_req("GET", "/api/me", {}, "wb_session=" + token),
                             c.complete()));
    assert(c.resp->status == 200);
    assert(c.resp->body.find("user@test.local") != std::string::npos);

    // /api/me 无 cookie → 401
    c = {};
    assert(fx.router->handle(make_req("GET", "/api/me"), c.complete()));
    assert(c.resp->status == 401);

    // 注销 → 200 + 清 cookie；旧 token 立即失效
    c = {};
    assert(fx.router->handle(make_req("POST", "/api/logout", {}, "wb_session=" + token),
                             c.complete()));
    assert(c.resp->status == 200);
    assert(c.resp->extra_headers.find("Max-Age=0") != std::string::npos);
    c = {};
    assert(fx.router->handle(make_req("GET", "/api/me", {}, "wb_session=" + token),
                             c.complete()));
    assert(c.resp->status == 401);
    std::cout << "  [PASS] router_login_flow" << std::endl;
}

// ========== MailService / mail_api（MockDbPool 延迟回调驱动）==========

using MockRows = std::vector<std::map<std::string, std::string>>;

struct MailApiFixture {
    std::shared_ptr<AuthCache> cache = std::make_shared<AuthCache>();
    SessionStore sessions{1000, std::chrono::seconds(60)};
    std::shared_ptr<test::MockDbPool> db = std::make_shared<test::MockDbPool>();
    std::shared_ptr<router::StaticShardRouter> shard;
    std::unique_ptr<AuthService> auth;
    std::unique_ptr<MailService> mail;
    std::unique_ptr<ApiRouter> router;
    const std::string token = SessionStore::generate_token();

    MailApiFixture() {
        AuthCacheEntry active;
        active.password_hash = bcrypt_hash("secret");
        active.status = 1;
        active.user_id = 42;
        cache->inject("user@test.local", active);

        shard = std::make_shared<router::StaticShardRouter>(
            std::vector<std::pair<std::string, int>>{}, 0,
            std::vector<std::shared_ptr<DBPool>>{db},
            std::vector<std::shared_ptr<pr::storage::IStorageProvider>>{});
        auth = std::make_unique<AuthService>(shard, cache, nullptr);
        mail = std::make_unique<MailService>(shard);
        router = std::make_unique<ApiRouter>(*auth, sessions, *mail);
        sessions.put(token, 42, "user@test.local");
        // MockDbConnection 默认 deferred：handle() 返回 true 但不回吐，
        // 测试逐发 fire_query 驱动 归属校验→count→分页 三段查询链
    }
};
static void test_mail_api_auth_required() {
    MailApiFixture fx;
    Captured c;

    // 无 cookie / 未知 token → 401，且不触 DB（零 pending 查询）
    assert(fx.router->handle(make_req("GET", "/api/mailboxes"), c.complete()));
    assert(c.resp->status == 401 && !fx.db->mock_conn()->has_pending_query());
    c = {};
    assert(fx.router->handle(make_req("GET", "/api/mailboxes/7/messages", {},
                                      "wb_session=deadbeef"),
                             c.complete()));
    assert(c.resp->status == 401 && !fx.db->mock_conn()->has_pending_query());
    std::cout << "  [PASS] mail_api_auth_required" << std::endl;
}

static void test_mail_api_mailboxes_list() {
    MailApiFixture fx;
    Captured c;
    assert(fx.router->handle(make_req("GET", "/api/mailboxes", {},
                                      "wb_session=" + fx.token),
                             c.complete()));
    assert(c.calls == 0);                       // deferred：未 fire 不回吐
    auto conn = fx.db->mock_conn();
    assert(conn->has_pending_query());
    assert((conn->last_query_params() ==
                std::vector<std::string>{"user@test.local", "42"}));

    conn->fire_query(std::make_shared<test::MockDbResult>(MockRows{
        {{"id", "1"}, {"name", "INBOX"}, {"box_type", "1"},
         {"total", "12"}, {"unread", "3"}},
        {{"id", "2"}, {"name", "Sent"}, {"box_type", "2"},
         {"total", "5"}, {"unread", "0"}}}));
    assert(c.calls == 1 && c.resp->status == 200);

    auto body = nlohmann::json::parse(c.resp->body);
    assert(body["mailboxes"].size() == 2);
    assert(body["mailboxes"][0]["id"] == 1);
    assert(body["mailboxes"][0]["name"] == "INBOX");
    assert(body["mailboxes"][0]["unread"] == 3);
    assert(body["mailboxes"][1]["box_type"] == 2);
    std::cout << "  [PASS] mail_api_mailboxes_list" << std::endl;
}

static void test_mail_api_messages_pagination() {
    MailApiFixture fx;
    Captured c;
    assert(fx.router->handle(
        make_req("GET", "/api/mailboxes/7/messages?page=2&page_size=10", {},
                 "wb_session=" + fx.token),
        c.complete()));
    auto conn = fx.db->mock_conn();

    // ① 邮箱归属校验（属于本人 → 继续）
    conn->fire_query(std::make_shared<test::MockDbResult>(MockRows{
        {{"id", "7"}, {"name", "INBOX"}, {"box_type", "1"}}}));
    assert(c.calls == 0 && conn->has_pending_query());

    // ② 总数
    conn->fire_query(std::make_shared<test::MockDbResult>(
        MockRows{{{"total", "25"}}}));
    assert(c.calls == 0 && conn->has_pending_query());

    // ③ 分页行：LIMIT/OFFSET 内联纯数字（10,10 = 第 2 页 × 每页 10）
    std::string sql = conn->last_query_sql();
    assert(sql.find("LIMIT 10 OFFSET 10") != std::string::npos);
    auto params = conn->last_query_params();
    assert(params.size() == 5);
    assert(params[0] == "user@test.local" && params[1] == "user@test.local" &&
           params[2] == "user@test.local" && params[3] == "7" && params[4] == "42");
    conn->fire_query(std::make_shared<test::MockDbResult>(MockRows{
        {{"id", "1001"}, {"subject", "Hello"}, {"sender", "alice@test.local"},
         {"recipients", ""}, {"send_time", "1729200000"}, {"status", "1"},
         {"is_starred", "1"}, {"is_important", "0"}, {"is_deleted", "0"},
         {"has_attachment", "1"}},
        {{"id", "1002"}, {"subject", "Ping"}, {"sender", ""},
         {"recipients", "bob@test.local,carol@test.local"},
         {"send_time", "1729100000"}, {"status", "0"},
         {"is_starred", "0"}, {"is_important", "0"}, {"is_deleted", "0"},
         {"has_attachment", "0"}}}));
    assert(c.calls == 1 && c.resp->status == 200);

    auto body = nlohmann::json::parse(c.resp->body);
    assert(body["mailbox"]["id"] == 7 && body["mailbox"]["name"] == "INBOX");
    assert(body["total"] == 25 && body["page"] == 2 && body["page_size"] == 10);
    assert(body["total_pages"] == 3);
    assert(body["messages"].size() == 2);
    assert(body["messages"][0]["sender"] == "alice@test.local");
    assert(body["messages"][0]["status"] == 1);
    assert(body["messages"][0]["is_starred"] == true);
    assert(body["messages"][0]["has_attachment"] == true);
    assert(body["messages"][1]["recipients"] ==
           "bob@test.local,carol@test.local");
    assert(body["messages"][1]["send_time"] == 1729100000);
    std::cout << "  [PASS] mail_api_messages_pagination" << std::endl;
}

static void test_mail_api_page_size_clamped() {
    MailApiFixture fx;
    Captured c;
    // page_size 超上限在 service 钳到 100（不 400——客户端只多要不多给）
    assert(fx.router->handle(
        make_req("GET", "/api/mailboxes/7/messages?page=1&page_size=500", {},
                 "wb_session=" + fx.token),
        c.complete()));
    auto conn = fx.db->mock_conn();
    conn->fire_query(std::make_shared<test::MockDbResult>(MockRows{
        {{"id", "7"}, {"name", "INBOX"}, {"box_type", "1"}}}));
    conn->fire_query(std::make_shared<test::MockDbResult>(
        MockRows{{{"total", "1"}}}));
    assert(conn->last_query_sql().find("LIMIT 100 OFFSET 0") != std::string::npos);
    conn->fire_query(std::make_shared<test::MockDbResult>(MockRows{
        {{"id", "1001"}, {"subject", "s"}, {"sender", "a@x"},
         {"recipients", ""}, {"send_time", "1"}, {"status", "0"},
         {"is_starred", "0"}, {"is_important", "0"}, {"is_deleted", "0"},
         {"has_attachment", "0"}}}));
    assert(c.calls == 1 && c.resp->status == 200);
    auto body = nlohmann::json::parse(c.resp->body);
    assert(body["page_size"] == 100);
    std::cout << "  [PASS] mail_api_page_size_clamped" << std::endl;
}

static void test_mail_api_mailbox_not_found() {
    MailApiFixture fx;
    Captured c;
    // 空结果 = 无此箱或不属于本人，同一口径 404（不给跨用户探测通道）
    assert(fx.router->handle(make_req("GET", "/api/mailboxes/99/messages", {},
                                      "wb_session=" + fx.token),
                             c.complete()));
    fx.db->mock_conn()->fire_query(std::make_shared<test::MockDbResult>());
    assert(c.calls == 1 && c.resp->status == 404);
    std::cout << "  [PASS] mail_api_mailbox_not_found" << std::endl;
}

static void mail_fire_one_row(MailApiFixture& fx) {
    auto conn = fx.db->mock_conn();
    conn->fire_query(std::make_shared<test::MockDbResult>(MockRows{
        {{"id", "7"}, {"name", "INBOX"}, {"box_type", "1"}}}));
    conn->fire_query(std::make_shared<test::MockDbResult>(
        MockRows{{{"total", "0"}}}));   // total=0 → 跳过分页查询，直接 200 空列表
}

static void test_mail_api_empty_mailbox() {
    MailApiFixture fx;
    Captured c;
    assert(fx.router->handle(make_req("GET", "/api/mailboxes/7/messages", {},
                                      "wb_session=" + fx.token),
                             c.complete()));
    mail_fire_one_row(fx);
    assert(c.calls == 1 && c.resp->status == 200);
    auto body = nlohmann::json::parse(c.resp->body);
    assert(body["total"] == 0 && body["total_pages"] == 0);
    assert(body["messages"].is_array() && body["messages"].empty());
    std::cout << "  [PASS] mail_api_empty_mailbox" << std::endl;
}

static void test_mail_api_db_down_fail_closed() {
    MailApiFixture fx;
    // 有池 + 空 mailbox 行 → 空邮箱列表也是合法 200
    Captured c;
    assert(fx.router->handle(make_req("GET", "/api/mailboxes", {},
                                      "wb_session=" + fx.token),
                             c.complete()));
    fx.db->mock_conn()->fire_query(std::make_shared<test::MockDbResult>());
    assert(c.calls == 1 && c.resp->status == 200);

    // DB 不可用（无池）→ 503 fail-closed，与登录同口径（ApiFixture 的
    // MailService 持 nullptr router）
    ApiFixture afx;
    std::string tok = SessionStore::generate_token();
    afx.sessions.put(tok, 42, "user@test.local");
    Captured c2;
    assert(afx.router->handle(make_req("GET", "/api/mailboxes", {},
                                       "wb_session=" + tok),
                              c2.complete()));
    assert(c2.calls == 1 && c2.resp->status == 503);
    c2 = {};
    assert(afx.router->handle(make_req("GET", "/api/mailboxes/7/messages", {},
                                       "wb_session=" + tok),
                              c2.complete()));
    assert(c2.calls == 1 && c2.resp->status == 503);
    std::cout << "  [PASS] mail_api_db_down_fail_closed" << std::endl;
}

static void test_mail_api_bad_requests() {
    MailApiFixture fx;
    Captured c;

    // 路径 id 非数字 → 未知路径 404（认领，不落静态）
    assert(fx.router->handle(make_req("GET", "/api/mailboxes/abc/messages", {},
                                      "wb_session=" + fx.token),
                             c.complete()));
    assert(c.resp->status == 404);

    // 分页参数非数字 / 0 → 400（先过会话，后校验参数）
    for (const char* q : {"?page=abc", "?page=1x", "?page=0", "?page_size=0",
                          "?page_size=-1", "?page=99999999999999999999"}) {
        c = {};
        assert(fx.router->handle(
            make_req("GET", std::string("/api/mailboxes/7/messages") + q, {},
                     "wb_session=" + fx.token),
            c.complete()));
        assert(c.resp->status == 400);
    }

    // 方法错 → 405
    c = {};
    assert(fx.router->handle(make_req("POST", "/api/mailboxes", {}, "wb_session=" + fx.token),
                             c.complete()));
    assert(c.resp->status == 405);
    c = {};
    assert(fx.router->handle(make_req("DELETE", "/api/mailboxes/7/messages", {},
                                      "wb_session=" + fx.token),
                             c.complete()));
    assert(c.resp->status == 405);

    // /api/mailboxes/7（无 /messages 后缀）→ 未知 API 404
    c = {};
    assert(fx.router->handle(make_req("GET", "/api/mailboxes/7", {}, "wb_session=" + fx.token),
                             c.complete()));
    assert(c.resp->status == 404);
    assert(!fx.db->mock_conn()->has_pending_query());
    std::cout << "  [PASS] mail_api_bad_requests" << std::endl;
}

int main() {
    std::cout << "Running webmail tests..." << std::endl;
    test_session_store_basics();
    test_session_store_ttl_expiry();
    test_cookie_parse();
    test_auth_service_outcomes();
    test_router_claim_semantics();
    test_router_login_flow();
    test_mail_api_auth_required();
    test_mail_api_mailboxes_list();
    test_mail_api_messages_pagination();
    test_mail_api_page_size_clamped();
    test_mail_api_mailbox_not_found();
    test_mail_api_empty_mailbox();
    test_mail_api_db_down_fail_closed();
    test_mail_api_bad_requests();
    std::cout << "All webmail tests passed!" << std::endl;
    return 0;
}
