// AuthCache TTL 语义回归：过期条目必须视为 miss（否则 DB 换密码永不起效）
// 2026-09-28：lookup 此前无视 LruCache 的 stale 标志，过期条目一直命中。
#include "mail_system/back/common/auth_cache.h"

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

using namespace mail_system;

int main() {
    // 1. 新鲜条目命中
    {
        AuthCache c(100, std::chrono::seconds(60));
        c.inject("a@t.local", {"pw-a", 1, 1, 0});
        AuthCacheEntry e;
        assert(c.lookup("a@t.local", e));
        assert(e.password_hash == "pw-a" && e.status == 1);
        AuthCacheEntry missing;
        assert(!c.lookup("nobody@t.local", missing));
        std::cout << "  [PASS] fresh hit / miss" << std::endl;
    }

    // 2. 过期视为 miss（核心回归：旧行为这里返回 true）
    {
        AuthCache c(100, std::chrono::seconds(1));
        c.inject("b@t.local", {"pw-b", 1, 2, 0});
        AuthCacheEntry e;
        assert(c.lookup("b@t.local", e));   // TTL 内命中
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        assert(!c.lookup("b@t.local", e));  // 过期 → miss，调用方走 DB 路径回填
        std::cout << "  [PASS] expired entry treated as miss" << std::endl;
    }

    // 3. 回填刷新时间戳：过期 → store → 再命中且拿到新值
    {
        AuthCache c(100, std::chrono::seconds(1));
        c.inject("c@t.local", {"old-pw", 1, 3, 0});
        AuthCacheEntry e;
        assert(c.lookup("c@t.local", e));
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        assert(!c.lookup("c@t.local", e));
        c.store("c@t.local", {"new-pw", 1, 3, 0});   // 模拟 DB 重查后回填
        assert(c.lookup("c@t.local", e));
        assert(e.password_hash == "new-pw");
        std::cout << "  [PASS] re-store refreshes timestamp and value" << std::endl;
    }

    // 4. 负缓存（禁用账号）同样受 TTL 约束：解禁最迟一个 TTL 生效
    {
        AuthCache c(100, std::chrono::seconds(1));
        c.inject("d@t.local", {"pw-d", 0, 4, 0});    // status=0 禁用
        AuthCacheEntry e;
        assert(c.lookup("d@t.local", e));            // TTL 内负缓存命中（快速拒绝）
        assert(e.status == 0);
        std::cout << "  [PASS] negative entry serves within TTL" << std::endl;
    }

    std::cout << "All auth_cache tests passed." << std::endl;
    return 0;
}
