#ifndef MAIL_SYSTEM_WEBMAIL_SESSION_STORE_H
#define MAIL_SYSTEM_WEBMAIL_SESSION_STORE_H

// ================================================================
// SessionStore — webmail 登录态（kickoff §三 目录结构）
//
// 为什么是 cookie 而不是 Authorization header（kickoff 设计决策 6）：
// JWT 放 Authorization 时 <a href> 下载链接带不上它；cookie 同源导航自动
// 携带，附件下载（带鉴权 handler 流式吐出）零额外机制。token 只存在于
// HttpOnly cookie —— 页面脚本读不到，XSS 偷不走会话。
//
// Secure 标志暂不默认加：本地/内网明文部署会直接失联，等 TLS 感知装配
// （listener 类型判断）落地后再补。
// ================================================================

#include "framework/cache/lru_cache.h"
#include "framework/http/http_types.hpp"
#include "mail_system/back/webmail/session_password.h"

#include <chrono>
#include <cstdint>
#include <openssl/rand.h>
#include <string>

namespace mail_system::webmail {

struct WebSessionInfo {
    uint64_t user_id = 0;
    std::string address;
};

// 单实例内存会话表。容量上限防登录风暴吃穿内存；TTL 到期条目按 LruCache
// 的 stale 语义判失效（get 返回 stale=true 视为未登录）。会话不落盘：
// 进程重启全员重新登录，与"踢会话全踢语义"（MVP-3）同一收益。
class SessionStore {
public:
    static constexpr std::chrono::seconds kDefaultTtl{24 * 3600};
    static constexpr const char* kCookieName = "wb_session";

    explicit SessionStore(size_t capacity = 100000,
                          std::chrono::seconds ttl = kDefaultTtl)
        : cache_(capacity, ttl), ttl_(ttl) {}

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
        cache_.put(token, Entry{{user_id, address}, std::move(password)});
    }

    // 命中且未过期返回 true。滑动续期 MVP 不做（固定 TTL，到期重新登录）。
    bool lookup(const std::string& token, WebSessionInfo& out,
                SessionPasswordPtr* password = nullptr) const {
        if (password) password->reset();
        if (token.empty()) return false;
        bool stale = false;
        Entry entry;
        if (!cache_.get(token, entry, stale) || stale) return false;
        out = entry.user;
        if (password) *password = entry.password;
        return true;
    }

    void erase(const std::string& token) { cache_.invalidate(token); }

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
};

} // namespace mail_system::webmail

#endif // MAIL_SYSTEM_WEBMAIL_SESSION_STORE_H
