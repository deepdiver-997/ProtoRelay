// webmail_test — webmail MVP 骨架单测（无 DB、无网络）
//   SessionStore：token 生成/登记/查询/注销/TTL 过期；cookie 解析
//   AuthService：缓存命中路径（预注入 AuthCache，无需 DB）+ 三态结果
//   ApiRouter：路由表派发（认领语义 / 405 / 404 / 登录全流程 / 会话自省）
#undef NDEBUG
#include <cassert>
#include <nlohmann/json.hpp>

#include "framework/http/http_types.hpp"
#include "mail_system/back/webmail/api/api_router.h"
#include "mail_system/back/webmail/auth_service.h"
#include "mail_system/back/webmail/session_store.h"
#include "mail_system/back/common/bcrypt.h"

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
        router = std::make_unique<ApiRouter>(*auth, sessions);
    }
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

int main() {
    std::cout << "Running webmail tests..." << std::endl;
    test_session_store_basics();
    test_session_store_ttl_expiry();
    test_cookie_parse();
    test_auth_service_outcomes();
    test_router_claim_semantics();
    test_router_login_flow();
    std::cout << "All webmail tests passed!" << std::endl;
    return 0;
}
