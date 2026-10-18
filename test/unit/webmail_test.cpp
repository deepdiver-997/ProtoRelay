// 2026-10-18: browser resource IDs lost precision and message detail omitted unread state.
// docs/bugfixes/2026-10-18-webmail-browser-contract.md
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
#include "mail_system/back/common/mime_composer.h"
#include "mail_system/back/algorithm/smtp_utils.h"
#include "mail_system/back/router/static_shard_router.h"
#include "mail_system/back/webmail/html_sanitize.h"
#include "mail_system/back/storage/local_file_storage_provider.h"
#include "mock_db_pool.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

using namespace mail_system;
using namespace mail_system::webmail;
namespace http = pr::http;
#include <unistd.h>

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
    assert(c.resp->body.find("\"id\":\"42\"") != std::string::npos);

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
    std::shared_ptr<storage::LocalFileStorageProvider> storage;
    std::shared_ptr<router::StaticShardRouter> shard;
    std::unique_ptr<AuthService> auth;
    std::unique_ptr<MailService> mail;
    std::unique_ptr<ApiRouter> router;
    const std::string token = SessionStore::generate_token();

    // storage_dir 非空时装配本地存储（读信全流程用）；空 = 无存储，
    // 读信走 body_available=false 降级路径
    explicit MailApiFixture(const std::string& storage_dir = {}) {
        AuthCacheEntry active;
        active.password_hash = bcrypt_hash("secret");
        active.status = 1;
        active.user_id = 42;
        cache->inject("user@test.local", active);

        std::vector<std::shared_ptr<pr::storage::IStorageProvider>> storages;
        if (!storage_dir.empty()) {
            std::filesystem::create_directories(storage_dir);
            storage = std::make_shared<storage::LocalFileStorageProvider>(
                storage_dir + "/", storage_dir + "/");
            storages.push_back(storage);
        }
        shard = std::make_shared<router::StaticShardRouter>(
            std::vector<std::pair<std::string, int>>{}, 0,
            std::vector<std::shared_ptr<DBPool>>{db}, std::move(storages));
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
    assert(body["mailboxes"][0]["id"] == "1");
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
    assert(body["mailbox"]["id"] == "7" && body["mailbox"]["name"] == "INBOX");
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

// ========== html_sanitize / mime_view ==========

static void test_html_sanitize_whitelist() {
    namespace hs = html_sanitize;
    // 脚本类元素连内容整块剥
    assert(hs::sanitize("<script>alert(1)</script>ok") == "ok");
    assert(hs::sanitize("<STYLE>body{}</STYLE>x") == "x");
    assert(hs::sanitize("<iframe src=\"http://e\">f</iframe>y") == "y");
    assert(hs::sanitize("<svg><circle/></svg>z") == "z");
    // 事件属性 / style 一律不留
    assert(hs::sanitize("<b onclick=\"x\" style=\"y\">hi</b>") == "<b>hi</b>");
    assert(hs::sanitize("<img src=\"http://track/x\" alt=\"pic\" width=\"5\">")
           == "<img alt=\"pic\" width=\"5\">");   // 远程图默认禁用（kickoff 决策 5）
    // href 白名单：http/https/mailto 放行，其余 scheme 剥属性
    assert(hs::sanitize("<a href=\"javascript:alert(1)\">x</a>") == "<a>x</a>");
    assert(hs::sanitize("<a href=\"data:text/html,evil\">x</a>") == "<a>x</a>");
    assert(hs::sanitize("<a href=\"https://a.b/c?d=1&e=2\">x</a>")
           == "<a href=\"https://a.b/c?d=1&amp;e=2\">x</a>");
    assert(hs::sanitize("<a href=\"mailto:a@b\">x</a>") == "<a href=\"mailto:a@b\">x</a>");
    // 注释 / DOCTYPE / PI 整段剥
    assert(hs::sanitize("<!-- c --><p>y</p>") == "<p>y</p>");
    assert(hs::sanitize("<!DOCTYPE html><p>y</p>") == "<p>y</p>");
    // 文本节点重新转义 + 字面 '<'
    assert(hs::sanitize("1 < 2 & 3") == "1 &lt; 2 &amp; 3");
    // 实体解码后过不了 scheme 白名单 → 剥（&#58; = ':'）
    assert(hs::sanitize("<a href=\"javascript&#58;alert(1)\">x</a>") == "<a>x</a>");
    // 恰好一次解码：已有实体不被二次转义（&amp; 进 &amp; 出）
    assert(hs::sanitize("<a href=\"https://a.b/?x=1&amp;y=2\">v</a>")
           == "<a href=\"https://a.b/?x=1&amp;y=2\">v</a>");
    assert(hs::sanitize("<p>a &amp; b &lt;c&gt;</p>") == "<p>a &amp; b &lt;c&gt;</p>");
    assert(hs::sanitize("&#65;&#x42;") == "AB");
    // 未闭合引号/未闭合标签：不挂不炸，整体剥
    assert(hs::sanitize("<a href=\"x").empty());
    assert(hs::sanitize("a < b <i>c</i>") == "a &lt; b <i>c</i>");
    // 白名单标签原样保留（重建形态）
    assert(hs::sanitize("<P CLASS=x>para</P>") == "<p>para</p>");
    std::cout << "  [PASS] html_sanitize_whitelist" << std::endl;
}

static void test_mime_view_decode() {
    namespace mv = mime_view;
    assert(mv::decode_header("plain subject") == "plain subject");
    assert(mv::decode_header("=?UTF-8?B?5L2g5aW9?=") == "你好");
    assert(mv::decode_header("=?UTF-8?B?5L2g?=\r\n =?UTF-8?B?5aW9?=") == "你好");
    assert(mv::decode_header("=?utf-8?q?hello_=E4=BD=A0=E5=A5=BD?=") == "hello 你好");
    assert(mv::decode_header("prefix =?utf-8?B?5L2g5aW9?= suffix") == "prefix 你好 suffix");
    assert(mv::decode_header("=?unknown?B?abcd?=") == "=?unknown?B?abcd?=");
    assert(mv::decode_header("=?broken") == "=?broken");
    // QP：=XX、软换行、小写 hex、非法序列按字面、尾部悬挂 '='
    assert(mv::decode_quoted_printable("h=C3=A9llo") == "h\xc3\xa9llo");
    assert(mv::decode_quoted_printable("a=\r\nb") == "ab");
    assert(mv::decode_quoted_printable("a=\nb") == "ab");
    assert(mv::decode_quoted_printable("=2f") == "/");
    assert(mv::decode_quoted_printable("a=zz") == "a=zz");
    assert(mv::decode_quoted_printable("end=") == "end=");
    // base64 part 解码（去行缀）
    MimePart p;
    p.encoding = "base64";
    auto d = mv::decode_part(outbound::base64_encode(
        reinterpret_cast<const unsigned char*>("<b>x</b>"), 8), p);
    assert(d.content == "<b>x</b>" && !d.truncated);
    // 7bit 原样
    p.encoding = "7bit";
    d = mv::decode_part("raw\r\n", p);
    assert(d.content == "raw\r\n");
    // 截断上限
    p.encoding = "7bit";
    d = mv::decode_part(std::string(mv::kMaxDecodedBytes + 10, 'a'), p);
    assert(d.truncated && d.content.size() == mv::kMaxDecodedBytes);
    // 叶子查找：multipart 树里第一个 text/<subtype>
    MimePart root;  root.type = "multipart"; root.subtype = "alternative";
    MimePart html;  html.type = "text"; html.subtype = "html";
    MimePart plain; plain.type = "text"; plain.subtype = "plain";
    MimePart mixed; mixed.type = "multipart"; mixed.subtype = "mixed";
    mixed.subs.push_back(plain);
    root.subs.push_back(html);
    root.subs.push_back(mixed);
    assert(mv::find_text_leaf(root, "html") == &root.subs[0]);
    assert(mv::find_text_leaf(root, "plain") == &root.subs[1].subs[0]);
    std::cout << "  [PASS] mime_view_decode" << std::endl;
}

// ========== 读信全流程（MockDbPool + 本地存储）==========

static void fire_message_meta(test::MockDbConnection& conn,
                              const std::string& body_path) {
    // ① 归属校验：mail 在箱内 + body_path
    conn.fire_query(std::make_shared<test::MockDbResult>(MockRows{
        {{"id", "7"}, {"subject", "Hello"}, {"body_path", body_path},
         {"send_time", "1729200000"}, {"status", "1"}, {"is_starred", "1"},
         {"is_important", "0"}, {"is_deleted", "0"}}}));
    // ② 发件人 / ③ 收件人 / ④ 附件（发起序 = 回调序，join 后进存储段）
    conn.fire_query(std::make_shared<test::MockDbResult>(MockRows{
        {{"sender", "alice@test.local"}}}));
    conn.fire_query(std::make_shared<test::MockDbResult>(MockRows{
        {{"recipient", "user@test.local"}},
        {{"recipient", "other@test.local"}}}));
    conn.fire_query(std::make_shared<test::MockDbResult>(MockRows{
        {{"id", "5"}, {"filename", "notes.txt"}, {"file_size", "42"},
         {"mime_type", "text/plain"}}}));
    // 存储读取是本地 provider 内联执行：④ 一落地响应即同步产出
}

static void test_mail_api_get_message_flow() {
    namespace fs = std::filesystem;
    const auto dir = (fs::temp_directory_path() /
                      ("webmail_test_" + std::to_string(::getpid()))).string();
    fs::remove_all(dir);
    MailApiFixture fx(dir);
    Captured c;

    // 组一封 multipart/alternative：QP 纯文本 + base64 HTML（含 script）
    const std::string html_raw = "<p>hi <script>alert(1)</script><b>bold</b></p>";
    const std::string html_b64 = outbound::base64_encode(
        reinterpret_cast<const unsigned char*>(html_raw.data()), html_raw.size());
    const std::string eml =
        "From: Alice <alice@test.local>\r\n"
        "To: user@test.local\r\n"
        "Subject: Hello\r\n"
        "MIME-Version: 1.0\r\n"
        "Content-Type: multipart/alternative; boundary=\"BB\"\r\n"
        "\r\n"
        "--BB\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Transfer-Encoding: quoted-printable\r\n"
        "\r\n"
        "h=C3=A9llo w=C3=B6rld=\r\n"
        "next line\r\n"
        "--BB\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Transfer-Encoding: base64\r\n"
        "\r\n"
        + html_b64 + "\r\n"
        "--BB--\r\n";
    const std::string body_path = dir + "/7.eml";
    { std::ofstream f(body_path, std::ios::binary); f << eml; }

    assert(fx.router->handle(make_req("GET", "/api/mailboxes/81/messages/7", {},
                                      "wb_session=" + fx.token),
                             c.complete()));
    auto conn = fx.db->mock_conn();
    fire_message_meta(*conn, body_path);
    assert(c.calls == 1 && c.resp->status == 200);

    auto body = nlohmann::json::parse(c.resp->body);
    assert(body["message"]["id"] == "7");
    assert(body["message"]["sender"] == "alice@test.local");
    assert(body["message"]["recipients"].size() == 2);
    assert(body["message"]["is_starred"] == true);
    // 2026-10-18: detail must expose unread state so the UI can mark it read.
    // docs/bugfixes/2026-10-18-webmail-browser-contract.md
    assert(body["message"]["status"] == 1);
    assert(body["body_available"] == true);
    // QP 解码（软换行 + UTF-8 字节）
    assert(body["body"]["text"] == "h\xc3\xa9llo w\xc3\xb6rldnext line");
    assert(body["body"]["charset_text"] == "utf-8");
    // base64 解码 + 白名单清洗：script 连内容消失，合法标签保留
    assert(body["body"]["html"] == "<p>hi <b>bold</b></p>");
    // 附件清单来自 attachments 表
    assert(body["attachments"].size() == 1);
    assert(body["attachments"][0]["filename"] == "notes.txt");

    // 再读一次走 sidecar 快路径（幂等，同样 200）
    c = {};
    assert(fx.router->handle(make_req("GET", "/api/mailboxes/81/messages/7", {},
                                      "wb_session=" + fx.token),
                             c.complete()));
    fire_message_meta(*conn, body_path);
    assert(c.calls == 1 && c.resp->status == 200);
    body = nlohmann::json::parse(c.resp->body);
    assert(body["body"]["html"] == "<p>hi <b>bold</b></p>");
    fs::remove_all(dir);
    std::cout << "  [PASS] mail_api_get_message_flow" << std::endl;
}

static void test_mail_api_get_message_degraded() {
    MailApiFixture fx;   // 无存储 → 信壳 200 + body_available=false
    Captured c;
    assert(fx.router->handle(make_req("GET", "/api/mailboxes/81/messages/7", {},
                                      "wb_session=" + fx.token),
                             c.complete()));
    fire_message_meta(*fx.db->mock_conn(), "");
    assert(c.calls == 1 && c.resp->status == 200);
    auto body = nlohmann::json::parse(c.resp->body);
    assert(body["body_available"] == false);
    assert(body["body"]["text_available"] == false);
    assert(body["attachments"].size() == 1);
    std::cout << "  [PASS] mail_api_get_message_degraded" << std::endl;
}

static void test_mail_api_get_message_denied() {
    MailApiFixture fx;
    Captured c;
    // 非本人/不在此箱/无此信 → 统一 404
    assert(fx.router->handle(make_req("GET", "/api/mailboxes/81/messages/7", {},
                                      "wb_session=" + fx.token),
                             c.complete()));
    fx.db->mock_conn()->fire_query(std::make_shared<test::MockDbResult>());
    assert(c.calls == 1 && c.resp->status == 404);
    // 未认证 → 401（零 DB 触达）
    c = {};
    assert(fx.router->handle(make_req("GET", "/api/mailboxes/81/messages/7"),
                             c.complete()));
    assert(c.resp->status == 401);
    // 路径 id 非数字 → 404
    c = {};
    assert(fx.router->handle(make_req("GET", "/api/mailboxes/81/messages/abc", {},
                                      "wb_session=" + fx.token),
                             c.complete()));
    assert(c.resp->status == 404);
    // POST → 405
    c = {};
    assert(fx.router->handle(make_req("POST", "/api/mailboxes/81/messages/7", {},
                                      "wb_session=" + fx.token),
                             c.complete()));
    assert(c.resp->status == 405);
    std::cout << "  [PASS] mail_api_get_message_denied" << std::endl;
}

struct MockSubmitter : ISmtpSubmitter {
    int calls = 0;
    SubmitRequest request;
    SubmitResult result{SubmitStatus::Accepted, 250, "body"};
    void submit_async(SubmitRequest input, Complete cb) override {
        ++calls;
        request = std::move(input);
        cb(result);
    }
};

static void test_compose_api() {
    ApiFixture fx;
    MockSubmitter submitter;
    ApiRouter router(*fx.auth, fx.sessions, *fx.mail, &submitter);
    const auto token = SessionStore::generate_token();
    auto password = std::make_shared<SessionPassword>("secret");
    fx.sessions.put(token, 42, "user@test.local", password);
    auto request = make_req("POST", "/api/send", R"({"to":["a@test.local"],"cc":["cc@test.local","a@test.local"],"subject":"你好","text":"hello\n.\nlast","html":"<b>hello</b>"})",
                            "wb_session=" + token);
    request.headers["content-type"] = "application/json; charset=utf-8";
    Captured c;
    assert(router.handle(request, c.complete()));
    assert(c.calls == 1 && c.resp->status == 202 && submitter.calls == 1);
    assert(submitter.request.sender == "user@test.local");
    assert(submitter.request.password->view() == "secret");
    assert(submitter.request.recipients == std::vector<std::string>({"a@test.local", "cc@test.local"}));
    assert(submitter.request.raw.find("From: <user@test.local>\r\n") != std::string::npos);
    assert(submitter.request.raw.find("Cc: <cc@test.local>") != std::string::npos);
    assert(submitter.request.raw.find("Bcc:") == std::string::npos);
    auto response = nlohmann::json::parse(c.resp->body);
    assert(response["status"] == "accepted");
    assert(submitter.request.raw.find(response["message_id"].get<std::string>()) != std::string::npos);

    // 站在现有 IMAP/webmail MIME 消费者视角验证能读回，两种正文都保留。
    MimePart root;
    parse_mime_tree(submitter.request.raw, root);
    const auto* text = mime_view::find_text_leaf(root, "plain");
    const auto* html = mime_view::find_text_leaf(root, "html");
    assert(text && html);
    assert(mime_view::decode_part(extract_part_content(submitter.request.raw, *text), *text).content == "hello\r\n.\r\nlast");
    assert(mime_view::decode_part(extract_part_content(submitter.request.raw, *html), *html).content == "<b>hello</b>");
    // 消费者应保留首尾空格，且用户输入的 encoded-word 字面量只解码一层。
    for (const std::string subject : {" plain ", "=?UTF-8?B?5L2g5aW9?=", "normal"}) {
        mime::Message message;
        message.from = "user@test.local";
        message.to = {"a@test.local"};
        message.subject = subject;
        message.text = "body";
        auto composed = mime::compose(message);
        auto headers = algorithm::parse_headers_map(composed.raw.substr(0, composed.raw.find("\r\n\r\n")));
        assert(mime_view::decode_header(headers.at("subject")) == subject);
    }

    // SMTP 返回分类：配额/策略拒绝，临时故障，DATA 后断连不声称成功或可重试。
    for (const auto& entry : std::vector<std::pair<SubmitStatus, int>>{
            {SubmitStatus::Rejected, 422}, {SubmitStatus::Unavailable, 503},
            {SubmitStatus::ProtocolError, 502}, {SubmitStatus::OutcomeUnknown, 504}}) {
        submitter.result = {entry.first, 550, "mail"};
        c = {};
        assert(router.handle(request, c.complete()));
        assert(c.calls == 1 && c.resp->status == entry.second);
        auto error = nlohmann::json::parse(c.resp->body);
        assert(error["retryable"] == (entry.first == SubmitStatus::Unavailable));
        assert(error["message_id"].is_string());
    }
    submitter.result = {SubmitStatus::AuthFailed, 535, "auth"};
    c = {};
    assert(router.handle(request, c.complete()));
    assert(c.resp->status == 401 && c.resp->extra_headers.find("Max-Age=0") != std::string::npos);
    WebSessionInfo info;
    assert(!fx.sessions.lookup(token, info));
    std::cout << "  [PASS] compose_api + MIME consumer roundtrip" << std::endl;
}

static void test_compose_api_rejections() {
    ApiFixture fx;
    MockSubmitter submitter;
    ApiRouter router(*fx.auth, fx.sessions, *fx.mail, &submitter);
    const auto token = SessionStore::generate_token();
    fx.sessions.put(token, 42, "user@test.local", std::make_shared<SessionPassword>("secret"));
    auto request = make_req("POST", "/api/send", R"({"to":["a@test.local"],"text":"ok"})", "wb_session=" + token);
    request.headers["content-type"] = "application/json";
    auto check = [&](http::HttpRequest req, int status) {
        Captured c;
        assert(router.handle(req, c.complete()));
        assert(c.calls == 1 && c.resp->status == status);
        assert(submitter.calls == 0);
    };
    auto req = request;
    req.headers.erase("cookie"); check(req, 401);
    req = request; req.method = "GET"; check(req, 405);
    req = request; req.headers.erase("content-type"); check(req, 415);
    req = request; req.headers["content-type"] = "text/plain"; check(req, 415);
    for (const std::string body : {"null", "[]", "{", R"({"from":"spoof@test.local","to":["a@test.local"],"text":"x"})",
            R"({"to":"a@test.local","text":"x"})", R"({"to":[4],"text":"x"})",
            R"({"to":["a@test.local"],"text":null})", R"({"to":["a@test.local"],"subject":"a\r\nBcc: bad@test.local","text":"x"})",
            R"({"to":["a@test.local>\r\nDATA"],"text":"x"})", R"({"to":[],"text":"x"})",
            R"({"to":["a@test.local"]})", R"({"to":["a@test.local"],"bcc":["hidden@test.local"],"text":"x"})", R"({"to":["a@test.local"],"attachments":[],"text":"x"})"}) {
        req = request; req.body = body; check(req, 400);
    }
    req = request;
    req.body = nlohmann::json{{"to", {"a@test.local"}}, {"text", std::string(mime::kMaxBodyBytes + 1, 'x')}}.dump();
    check(req, 413);
    req = request; req.body.assign(1024 * 1024 + 1, 'x'); check(req, 413);
    // 登录成功后 SMTP 凭据留在会话中，me 不泄漏密码。
    Captured login;
    assert(router.handle(make_req("POST", "/api/login", R"({"address":"user@test.local","password":"secret"})"), login.complete()));
    assert(login.resp->status == 200 && login.resp->body.find("secret") == std::string::npos);
    auto cookie = login.resp->extra_headers.substr(strlen("Set-Cookie: "));
    auto authenticated = make_req("GET", "/api/me", {}, cookie);
    WebSessionInfo info;
    SessionPasswordPtr password;
    assert(fx.sessions.lookup(SessionStore::token_from_request(authenticated), info, &password));
    assert(password && password->view() == "secret");
    Captured me;
    assert(router.handle(authenticated, me.complete()));
    assert(me.resp->body.find("secret") == std::string::npos);
    // 过期/注销永远取不到可提交凭据。
    SessionStore expired(10, std::chrono::seconds(0));
    expired.put("expired", 42, info.address, password);
    assert(!expired.lookup("expired", info, &password) && !password);
    std::cout << "  [PASS] compose_api_rejections" << std::endl;
}

// 2026-10-18: cross-node sessions, DB outage and stale-login credential regression.
// Real two-process negative/positive verification: docs/bugfixes/2026-10-18-webmail-shared-sessions.md.
static void test_database_sessions_and_fresh_auth() {
    auto pool = std::make_shared<test::MockDbPool>();
    auto router = std::make_shared<router::StaticShardRouter>(std::vector<std::pair<std::string,int>>{}, 0,
        std::vector<std::shared_ptr<DBPool>>{pool}, std::vector<std::shared_ptr<storage::IStorageProvider>>{});
    auto conn = pool->mock_conn();
    SessionStore sessions(router, nullptr);
    auto token = SessionStore::generate_token();
    auto registry = [] {
        return std::make_shared<test::MockDbResult>(std::vector<std::map<std::string,std::string>>{
            {{"user_id","42"},{"account_address","user@test.local"},{"account_shard","0"},
             {"credential_stamp",credential_stamp("stored-credential")}}});
    };
    auto user = [](std::string status, std::string password) {
        return std::make_shared<test::MockDbResult>(std::vector<std::map<std::string,std::string>>{
            {{"id","42"},{"status",std::move(status)},{"password",std::move(password)}}});
    };
    int calls = 0;
    conn->push_sync_result(registry()); conn->push_sync_result(user("1", "stored-credential"));
    sessions.lookup_async(token, [&](SessionStore::LookupResult r) { ++calls; assert(r.found && !r.server_error && !r.password); assert(r.user.user_id == 42); });
    assert(calls == 1);
    for (auto row : {user("0", "stored-credential"), user("1", "changed-credential")}) {
        conn->push_sync_result(registry()); conn->push_sync_result(row);
        sessions.lookup_async(token, [&](SessionStore::LookupResult r) { ++calls; assert(!r.found && !r.server_error); });
    }
    conn->push_sync_result(nullptr);
    sessions.lookup_async(token, [&](SessionStore::LookupResult r) { ++calls; assert(!r.found && r.server_error); });
    conn->push_sync_result(registry()); conn->push_sync_result(nullptr);
    sessions.lookup_async(token, [&](SessionStore::LookupResult r) { ++calls; assert(!r.found && r.server_error); });
    sessions.lookup_async("malformed", [&](SessionStore::LookupResult r) { ++calls; assert(!r.found && !r.server_error); });
    assert(calls == 6);

    // Fresh web login must ignore a cached old credential and fail closed on DB errors.
    auto cache = std::make_shared<AuthCache>();
    cache->inject("user@test.local", {"old-password", 1, 42, 0});
    AuthService auth(router, cache, nullptr, true);
    bool completed = false;
    auth.login_async("user@test.local", "old-password", [&](AuthOutcome r) { completed = true; assert(!r.ok && !r.server_error); });
    assert(!completed && conn->has_pending_query());
    conn->fire_query(user("1", bcrypt_hash("new-password")));
    assert(completed);
    completed = false;
    auth.login_async("user@test.local", "old-password", [&](AuthOutcome r) { completed = true; assert(!r.ok && r.server_error); });
    conn->fire_query(nullptr);
    assert(completed);

    // A password reset winning the legacy rehash CAS cannot mint a session for the old password.
    completed = false;
    auth.login_async("user@test.local", "legacy-password", [&](AuthOutcome r) { completed = true; assert(!r.ok); });
    conn->fire_query(user("1", "legacy-password"));
    assert(conn->has_pending_execute());
    conn->fire_execute(true); // CAS success shape also includes zero affected rows.
    assert(conn->has_pending_query());
    conn->fire_query(user("1", bcrypt_hash("replacement-password")));
    assert(completed && !conn->has_pending_execute()); // failed revalidation does not update last_login

    // Protected HTTP endpoints distinguish unavailable DB (503) from unknown tokens (401).
    MailService mail(router);
    ApiRouter api(auth, sessions, mail);
    conn->push_sync_result(nullptr);
    Captured cap;
    assert(api.handle(make_req("GET", "/api/me", {}, "wb_session=" + token), cap.complete()));
    assert(cap.calls == 1 && cap.resp->status == 503);
    SessionStore unavailable(nullptr, nullptr);
    Captured logout_result;
    api::logout(make_req("POST", "/api/logout", "{}", "wb_session=" + token), unavailable, logout_result.complete());
    assert(logout_result.calls == 1 && logout_result.resp->status == 503);
    bool issued = true;
    unavailable.put_async(token, {42, "user@test.local", credential_stamp("stored"), 0}, nullptr,
                          [&](bool ok) { issued = ok; });
    assert(!issued);
}

// 2026-10-18: a new login must revoke the previous token, including stale logout races.
// Evidence: docs/bugfixes/2026-10-18-webmail-single-login.md.
static void test_single_login_replacement() {
    SessionStore store;
    auto first = SessionStore::generate_token();
    auto second = SessionStore::generate_token();
    auto other = SessionStore::generate_token();
    WebSessionInfo user;
    store.put(first, 42, "user@test.local");
    store.put(other, 43, "other@test.local");
    store.put(second, 42, "user@test.local");
    assert(!store.lookup(first, user));
    assert(store.lookup(second, user) && user.user_id == 42);
    assert(store.lookup(other, user) && user.user_id == 43);
    store.erase(first); // stale device's logout cannot revoke the new login
    assert(store.lookup(second, user));
    auto other_shard = SessionStore::generate_token();
    bool stored = false;
    store.put_async(other_shard, {42, "user@other-shard.local", {}, 1}, nullptr, [&](bool ok) { stored = ok; });
    assert(stored && store.lookup(other_shard, user) && user.shard == 1);
    assert(store.lookup(second, user)); // IDs can repeat on different account shards
    store.erase(second);
    assert(!store.lookup(second, user));

    auto pool = std::make_shared<test::MockDbPool>();
    auto router = std::make_shared<router::StaticShardRouter>(std::vector<std::pair<std::string,int>>{}, 0,
        std::vector<std::shared_ptr<DBPool>>{pool}, std::vector<std::shared_ptr<storage::IStorageProvider>>{});
    SessionStore database(router, nullptr);
    auto index = [](std::string unique_flag) {
        return std::make_shared<test::MockDbResult>(std::vector<std::map<std::string,std::string>>{
            {{"Non_unique",unique_flag},{"Seq_in_index","1"},{"Column_name","account_shard"}},
            {{"Non_unique",unique_flag},{"Seq_in_index","2"},{"Column_name","user_id"}}});
    };
    // Missing/incorrect schema or metadata-query outage must fail closed before issuing a cookie.
    for (auto rows : {std::shared_ptr<IDBResult>{}, std::shared_ptr<IDBResult>(std::make_shared<test::MockDbResult>()),
                      std::shared_ptr<IDBResult>(index("1"))}) {
        pool->mock_conn()->push_sync_result(rows);
        int calls = 0;
        database.put_async(first, {42, "user@test.local", credential_stamp("stored"), 0}, nullptr,
            [&](bool ok) { ++calls; assert(!ok); });
        assert(calls == 1);
    }
    pool->mock_conn()->push_sync_result(index("0"));
    int calls = 0;
    database.put_async(second, {42, "user@test.local", credential_stamp("stored"), 0}, nullptr,
        [&](bool ok) { ++calls; assert(ok); });
    assert(calls == 1);
}

// 2026-10-18: JSON numeric Snowflake IDs lose precision in JavaScript.
// See docs/bugfixes/2026-10-18-webmail-browser-contract.md.
static void test_resource_ids_are_lossless() {
    auto response = api::json_response(200, {{"message", {{"id", uint64_t{9007199254740993ULL}}, {"status", 1}}}});
    auto body = nlohmann::json::parse(response.body);
    assert(body["message"]["id"].is_string());
    assert(body["message"]["id"] == "9007199254740993");
    assert(body["message"]["status"] == 1);
}


static void test_message_actions_contract() {
    const auto dir = std::filesystem::temp_directory_path() / ("webmail-actions-" + std::to_string(getpid()));
    std::filesystem::remove_all(dir);
    MailApiFixture fx(dir.string());
    MailActions actions(fx.shard, nullptr);
    ApiRouter api(*fx.auth, fx.sessions, *fx.mail, nullptr, &actions);
    const auto resource = "/api/mailboxes/81/messages/7";
    auto check = [&](http::HttpRequest req, int status) {
        Captured response;
        assert(api.handle(req, response.complete()));
        assert(response.calls == 1 && response.resp->status == status);
        return response.resp;
    };
    check(make_req("PATCH", resource, "{\"read\":true}"), 401);
    for (const auto& body : {"{}", "[]", "{", "{\"read\":1}", "{\"deleted\":true}"}) {
        auto req = make_req("PATCH", resource, body, "wb_session=" + fx.token);
        req.headers["content-type"] = "application/json";
        check(std::move(req), 400);
    }
    auto req = make_req("PATCH", resource, "{\"read\":true}", "wb_session=" + fx.token);
    check(req, 415);
    req.headers["content-type"] = "application/json";
    check(req, 404); // no ownership row
    fx.db->mock_conn()->push_sync_result(nullptr);
    check(req, 503);
    fx.db->mock_conn()->push_sync_result(std::make_shared<test::MockDbResult>(MockRows{{{"box_type","1"}}}));
    check(req, 200);
    fx.db->mock_conn()->push_sync_result(std::make_shared<test::MockDbResult>(MockRows{{{"box_type","1"}}}));
    check(req, 200); // repeat is idempotent
    const auto file = (dir / "binary").string();
    { std::ofstream out(file, std::ios::binary); out.write("a\0b", 3); }
    const std::string download = std::string(resource) + "/attachments/5";
    check(make_req("GET", download, {}, "wb_session=" + fx.token), 404);
    for (const auto& method : {"GET", "HEAD"}) {
        fx.db->mock_conn()->push_sync_result(std::make_shared<test::MockDbResult>(MockRows{
            {{"filepath",file},{"filename","hello\"\r\nInjected: yes.txt"}}}));
        auto response = check(make_req(method, download, {}, "wb_session=" + fx.token), 200);
        assert(response->content_length == 3 && response->body.empty());
        assert(response->extra_headers.find("%22%0D%0AInjected%3A%20yes.txt") != std::string::npos);
        assert(response->extra_headers.find("\r\nInjected:") == std::string::npos);
        assert(bool(response->body_source) == (std::string(method) == "GET"));
        if (response->body_source) {
            char bytes[8]; size_t count = 0; pr::IoError error;
            assert(response->body_source->read_at(0, bytes, sizeof(bytes), count, error));
            assert(std::string(bytes,count) == std::string("a\0b",3));
        }
    }
    fx.db->mock_conn()->push_sync_result(std::make_shared<test::MockDbResult>(MockRows{{{"filepath",file+"missing"}}}));
    check(make_req("GET", download, {}, "wb_session=" + fx.token), 503);
    std::filesystem::remove_all(dir);
    std::cout << "  [PASS] flags and attachment API validation, ownership, errors, binary stream and safe filename\n";
}

void run_webmail_submission_tests();

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--unread-detail-regression") {
        test_mail_api_get_message_flow(); return 0;
    }
    test_resource_ids_are_lossless();
    test_message_actions_contract();
    if (argc == 2 && std::string(argv[1]) == "--single-login-regression") {
        test_single_login_replacement(); return 0;
    }
    test_single_login_replacement();
    if (argc == 2 && std::string(argv[1]) == "--auth-cache-regression") {
        test_database_sessions_and_fresh_auth(); return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--auth-worker-regression") {
        run_webmail_submission_tests(); return 0;
    }
    run_webmail_submission_tests();
    test_database_sessions_and_fresh_auth();
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
    test_html_sanitize_whitelist();
    test_mime_view_decode();
    test_mail_api_get_message_flow();
    test_mail_api_get_message_degraded();
    test_mail_api_get_message_denied();
    test_compose_api();
    test_compose_api_rejections();
    std::cout << "All webmail tests passed!" << std::endl;
    return 0;
}
