#pragma once
#include "framework/cache/lru_cache.h"
#include "framework/http/http_types.hpp"
#include "mail_system/back/webmail/session_password.h"

#include "mail_system/back/router/i_shard_router.h"
#include "framework/thread_pool/thread_pool_base.h"
#include <functional>
#include <chrono>
#include <cstdint>
#include <openssl/rand.h>
#include <string>

namespace mail_system::webmail {

struct WebSessionInfo {
    uint64_t user_id = 0;
    std::string address;
    std::string credential_stamp;
    int shard = 0;
};

// 生产装配使用共享 DB；内存构造仅供嵌入和测试。固定 TTL，不滑动续期。
class SessionStore {
public:
    static constexpr std::chrono::seconds kDefaultTtl{24 * 3600};
    static constexpr const char* kCookieName = "wb_session";

    explicit SessionStore(size_t capacity = 100000,
                          std::chrono::seconds ttl = kDefaultTtl)
        : cache_(capacity, ttl), ttl_(ttl) {}

    SessionStore(std::shared_ptr<router::IShardRouter> router,
                 std::shared_ptr<pr::ThreadPoolBase> workers,
                 std::chrono::seconds ttl = kDefaultTtl)
        : cache_(1, ttl), ttl_(ttl), router_(std::move(router)), workers_(std::move(workers)), database_(true) {}
    struct LookupResult {
        bool found = false;
        bool server_error = false;
        WebSessionInfo user;
        SessionPasswordPtr password; // only explicit in-memory/loopback compatibility
    };
    bool database_backed() const { return database_; }
    void put_async(std::string token, WebSessionInfo user, SessionPasswordPtr password, std::function<void(bool)> cb);
    void lookup_async(std::string token, std::function<void(LookupResult)> cb) const;
    void erase_async(std::string token, std::function<void(bool)> cb);
    std::chrono::seconds ttl() const { return ttl_; }

    // 256 位 CSPRNG token，hex 编码 64 字符。RAND_bytes 失败返回空串
    //（调用方按 500 处理，绝不降级到弱随机源）。
    static std::string generate_token() {
        unsigned char raw[32];
        if (RAND_bytes(raw, sizeof(raw)) != 1) return {};
        static const char* hex = "0123456789abcdef";
        std::string out;
        out.reserve(sizeof(raw) * 2);
        for (unsigned char b : raw) {
            out.push_back(hex[b >> 4]);
            out.push_back(hex[b & 0x0f]);
        }
        return out;
    }

    void put(const std::string& token, uint64_t user_id, const std::string& address,
             SessionPasswordPtr password = nullptr) {
        if (database_) throw std::logic_error("use async DB session API");
        cache_.put(token, Entry{{user_id, address, {}, 0}, std::move(password)});
    }

    // 命中且未过期返回 true。滑动续期 MVP 不做（固定 TTL，到期重新登录）。
    bool lookup(const std::string& token, WebSessionInfo& out,
                SessionPasswordPtr* password = nullptr) const {
        if (password) password->reset();
        if (database_ || token.empty()) return false;
        bool stale = false;
        Entry entry;
        if (!cache_.get(token, entry, stale) || stale) return false;
        out = entry.user;
        if (password) *password = entry.password;
        return true;
    }

    void erase(const std::string& token) {
        if (database_) throw std::logic_error("use async DB session API");
        cache_.invalidate(token);
    }

    size_t size() const { return cache_.size(); }

    // ── cookie 帮助 ─────────────────────────────────────────────
    // 登录成功：HttpOnly + SameSite=Lax（kickoff 设计决策 6）。
    std::string issue_cookie_header(const std::string& token) const {
        return std::string("Set-Cookie: ") + kCookieName + "=" + token +
               "; Path=/; HttpOnly; SameSite=Lax; Max-Age=" +
               std::to_string(ttl_.count()) + "\r\n";
    }

    // 注销：Max-Age=0 让浏览器立刻删 cookie。
    static std::string clear_cookie_header() {
        return std::string("Set-Cookie: ") + kCookieName +
               "=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0\r\n";
    }

    // 从请求 Cookie 头里取会话 token；无 cookie / 无本会话名返回空串。
    static std::string token_from_request(const pr::http::HttpRequest& req) {
        const std::string* cookie = req.find_header("cookie");
        if (!cookie) return {};
        const std::string key = std::string(kCookieName) + "=";
        size_t pos = 0;
        while (true) {
            pos = cookie->find(key, pos);
            if (pos == std::string::npos) return {};
            // 前缀必须独立成对（行首或 "; " 之后），防止 xwb_session= 之类误命中
            if (pos != 0 && (*cookie)[pos - 1] != ' ' && (*cookie)[pos - 1] != ';') {
                pos += key.size();
                continue;
            }
            size_t start = pos + key.size();
            size_t end = cookie->find(';', start);
            return cookie->substr(start, end == std::string::npos ? std::string::npos
                                                                  : end - start);
        }
    }

private:
    struct Entry {
        WebSessionInfo user;
        SessionPasswordPtr password;
    };
    pr::LruCache<std::string, Entry> cache_;
    std::chrono::seconds ttl_;
    std::shared_ptr<router::IShardRouter> router_;
    std::shared_ptr<pr::ThreadPoolBase> workers_;
    bool database_ = false;
};

} // namespace mail_system::webmail
