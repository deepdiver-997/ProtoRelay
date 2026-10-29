#include "test_temp_dir.h"
// pr::IntrusionDetector 单元测试：会话记录、私网过滤、封禁阈值、LRU 上限、持久化。
//
// 生产里它跑在 server_base 的会话结束路径上，此前无直接测试。
// 注意两点测试约束：
//   1) LRU 驱逐按 last_seen（秒级墙钟）比较，同秒插入的记录 last_seen 相同，
//      驱逐对象不可预测 —— 只断言记录数有界，不断言具体被驱逐的 IP。
//   2) 懒刷盘的触发条件是「记录数 ≥ 脏阈值 且 (首刷或已过刷盘间隔)」，
//      测试把 interval 设 0 保证第二次记录即触发。
#undef NDEBUG
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "framework/intrusion_detector.h"

namespace {

using pr::IntrusionDetector;

int g_pass = 0, g_fail = 0;
void expect_true(bool c, const char* what) {
    if (c) { g_pass++; }
    else { g_fail++; std::printf("  FAIL %s\n", what); }
}

std::filesystem::path tmp_dir() {
    return test_support::temp_root() / "protorelay_intrusion_test";
}

} // namespace

int main() {
    std::printf("intrusion_detector_test\n");
    auto dir = tmp_dir();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    // 1. 未 enable：record_session 不记录
    {
        pr::IntrusionDetector d;
        d.record_session("8.8.8.8", false);
        expect_true(d.size() == 0, "disabled detector records nothing");
        expect_true(!d.is_enabled(), "disabled by default");
    }

    // 2. 空 IP：即便 enable 也不记录
    {
        pr::IntrusionDetector d;
        d.set_enabled(true);
        d.record_session("", false);
        expect_true(d.size() == 0, "empty ip ignored");
    }

    // 3. 私网 IP 全部跳过，公网 IP 正常记录
    {
        pr::IntrusionDetector d;
        d.set_enabled(true);
        const char* privates[] = {"127.0.0.1", "::1", "0.0.0.0",
                                  "10.1.2.3", "192.168.0.1",
                                  "172.16.0.1", "172.31.255.255"};
        for (auto ip : privates) d.record_session(ip, false);
        expect_true(d.size() == 0, "private/loopback ips skipped");

        d.record_session("172.32.0.1", false);   // 172.32 不在 16-31 区间
        d.record_session("172.15.0.1", false);   // 172.15 不在 16-31 区间
        d.record_session("8.8.8.8", false);
        expect_true(d.size() == 3, "public ips recorded");
    }

    // 4. 计数：total_connections 累加，failed_count 仅未认证时累加
    {
        pr::IntrusionDetector d;
        d.set_enabled(true);
        d.record_session("8.8.8.8", false);
        d.record_session("8.8.8.8", false);
        d.record_session("8.8.8.8", true);
        auto r = d.query("8.8.8.8");
        expect_true(r.total_connections == 3, "total_connections increments each session");
        expect_true(r.failed_count == 2, "failed_count only counts unauthenticated");
        expect_true(r.first_seen != 0 && r.last_seen != 0, "timestamps populated");
    }

    // 5. 封禁阈值：默认 0 永不放行；设阈值后达标放行
    {
        pr::IntrusionDetector d;
        d.set_enabled(true);
        for (int i = 0; i < 5; i++) d.record_session("1.1.1.1", false);
        expect_true(!d.is_banned("1.1.1.1"), "ban_threshold=0 never bans");
        expect_true(!d.is_banned("8.8.8.8"), "unknown ip never banned");

        d.set_ban_threshold(3);
        expect_true(d.is_banned("1.1.1.1"), "failed_count>=threshold bans");
        expect_true(!d.is_banned("9.9.9.9"), "sub-threshold ip not banned");

        pr::IntrusionDetector d2;
        d2.set_enabled(true);
        d2.set_ban_threshold(3);
        for (int i = 0; i < 2; i++) d2.record_session("2.2.2.2", false);
        expect_true(!d2.is_banned("2.2.2.2"), "below threshold not banned");
    }

    // 6. LRU 上限：超 max_records 后记录数有界
    {
        pr::IntrusionDetector d;
        d.set_enabled(true);
        d.set_max_records(5);
        for (int i = 0; i < 50; i++)
            d.record_session("ip-" + std::to_string(i) + ".example", false);
        expect_true(d.size() == 5, "records capped at max_records");
        expect_true(d.snapshot().size() == 5, "snapshot reflects cap");
    }

    // 7. persist / restore 往返一致
    {
        auto file = (dir / "detector.json").string();
        {
            pr::IntrusionDetector d(file);
            d.set_enabled(true);
            d.record_session("8.8.8.8", false);
            d.record_session("1.1.1.1", true);
            expect_true(d.persist(), "persist returns true");
        }
        pr::IntrusionDetector d2(file);
        expect_true(d2.restore(), "restore returns true");
        expect_true(d2.size() == 2, "restored record count");
        expect_true(d2.query("8.8.8.8").failed_count == 1, "restored failed_count");
        expect_true(d2.query("1.1.1.1").failed_count == 0, "restored authenticated record");
    }

    // 8. 懒刷盘：脏阈值 2 + 间隔 0 → 第二次记录即落盘
    {
        auto file = (dir / "lazy.json").string();
        {
            pr::IntrusionDetector d(file);
            d.set_enabled(true);
            d.set_persist_dirty_threshold(2);
            d.set_persist_interval(0);
            d.record_session("8.8.8.8", false);
            expect_true(std::filesystem::exists(file) == false,
                        "no persist before dirty threshold");
            d.record_session("1.1.1.1", false);
            expect_true(std::filesystem::exists(file),
                        "lazy persist fires at dirty threshold");
        }
    }

    // 9. 空 data_file：persist/restore 返回 false
    {
        pr::IntrusionDetector d("");
        d.set_enabled(true);
        d.record_session("8.8.8.8", false);
        expect_true(!d.persist(), "empty data_file persist -> false");
        pr::IntrusionDetector d2("");
        expect_true(!d2.restore(), "empty data_file restore -> false");
    }

    // 10. setter 生效
    {
        pr::IntrusionDetector d;
        d.set_max_records(7);
        d.set_ban_threshold(4);
        d.set_persist_interval(30);
        d.set_persist_dirty_threshold(10);
        d.set_enabled(true);
        for (int i = 0; i < 9; i++) d.record_session("x" + std::to_string(i), false);
        expect_true(d.size() == 7, "max_records setter caps size");
    }

    // 11. 失败窗口：窗口过期后 failed_count 清零重新累计
    {
        pr::IntrusionDetector d;
        d.set_enabled(true);
        uint64_t now = 1000;
        d.set_now_source_for_test([&now] { return now; });
        d.set_ban_threshold(3);
        d.set_fail_window_sec(10);
        d.record_session("3.3.3.3", false);
        d.record_session("3.3.3.3", false);
        now = 1020;  // 已过 10s 窗口
        d.record_session("3.3.3.3", false);
        expect_true(d.query("3.3.3.3").failed_count == 1, "window expiry resets failed_count");
        expect_true(!d.is_banned("3.3.3.3"), "post-reset count below threshold");
    }

    // 12. 时间盒封禁：达标即封、到期自动解封、窗口配合下可持续再封
    {
        pr::IntrusionDetector d;
        d.set_enabled(true);
        uint64_t now = 1000;
        d.set_now_source_for_test([&now] { return now; });
        d.set_ban_threshold(3);
        d.set_fail_window_sec(10);
        d.set_ban_duration_sec(50);
        bool b3 = false;
        for (int i = 0; i < 3; i++) b3 = d.record_session("4.4.4.4", false);
        expect_true(b3, "third failure returns just_banned");
        expect_true(d.is_banned("4.4.4.4"), "time-boxed ban active");
        expect_true(d.query("4.4.4.4").banned_until == 1050, "banned_until = now+duration");

        now = 1060;  // 封禁已过期
        expect_true(!d.is_banned("4.4.4.4"), "ban expires after duration");
        d.record_session("4.4.4.4", false);  // 窗口过期 → 计数清 1，过期 banned_until 清 0
        expect_true(d.query("4.4.4.4").failed_count == 1, "window reset after ban expiry");
        d.record_session("4.4.4.4", false);
        bool re_banned = d.record_session("4.4.4.4", false);
        expect_true(re_banned, "sustained attack re-banned");
        expect_true(d.query("4.4.4.4").banned_until == 1060 + 50, "re-ban extends banned_until");
    }

    // 13. 黑名单：精确 + CIDR，优先级高于白名单，注释/空行容忍
    {
        auto bl = (dir / "blacklist.txt").string();
        {
            std::ofstream ofs(bl);
            ofs << "# 手工黑名单\n\ngoogle-scan.example\n62.60.134.0/24\n";
        }
        pr::IntrusionDetector d;
        d.set_enabled(true);
        d.set_blacklist_file(bl);
        expect_true(d.blacklist_size() == 2, "blacklist entries parsed (comment/blank skipped)");
        expect_true(d.is_banned("62.60.134.211"), "CIDR /24 match");
        expect_true(d.is_banned("62.60.134.1"), "CIDR network addr match");
        expect_true(!d.is_banned("62.60.135.1"), "outside CIDR not banned");
        d.record_session("6.6.6.6", false);
        d.set_whitelist({"6.6.6.6"});
        d.set_blacklist_file(bl);  // 重新加载不影响既有白名单
        expect_true(!d.is_banned("6.6.6.6"), "whitelisted ip not banned");
        d.set_whitelist({"8.8.8.8", "62.60.134.200"});
        expect_true(d.is_banned("google-scan.example"), "exact blacklist entry");
        expect_true(d.is_banned("62.60.134.200"), "blacklist wins over whitelist");
        expect_true(!d.is_banned("8.8.8.8"), "whitelist-only ip not banned");
    }

    // 14. 黑名单 mtime 自动重载（节流窗口过后才 stat）
    {
        auto bl = (dir / "blacklist_reload.txt").string();
        { std::ofstream ofs(bl); ofs << "9.9.9.9\n"; }
        pr::IntrusionDetector d;
        d.set_enabled(true);
        uint64_t now = 2000;
        d.set_now_source_for_test([&now] { return now; });
        d.set_blacklist_file(bl);
        expect_true(d.is_banned("9.9.9.9"), "blacklist loaded on set");

        { std::ofstream ofs(bl, std::ios::trunc); ofs << "# emptied\n"; }
        std::filesystem::last_write_time(bl,
            std::filesystem::file_time_type::clock::now() + std::chrono::hours(1));
        expect_true(d.is_banned("9.9.9.9"), "throttled: no reload within 10s");
        now = 2011;
        expect_true(!d.is_banned("9.9.9.9"), "mtime change triggers reload");
    }

    // 15. 白名单：不记账、永不封禁
    {
        pr::IntrusionDetector d;
        d.set_enabled(true);
        d.set_ban_threshold(2);
        d.set_whitelist({"5.5.5.5"});
        for (int i = 0; i < 10; i++) d.record_session("5.5.5.5", false);
        expect_true(d.size() == 0, "whitelisted ip not accounted");
        expect_true(!d.is_banned("5.5.5.5"), "whitelisted ip never banned");
    }

    // 16. LRU 不驱逐封禁期内的记录
    {
        pr::IntrusionDetector d;
        d.set_enabled(true);
        d.set_ban_threshold(1);
        d.set_ban_duration_sec(1000);
        d.set_max_records(2);
        d.record_session("banned.example", false);
        expect_true(d.is_banned("banned.example"), "seeded ban");
        for (int i = 0; i < 10; i++)
            d.record_session("other-" + std::to_string(i) + ".example", true);
        expect_true(d.query("banned.example").banned_until != 0,
                    "banned record survives LRU eviction");
        expect_true(d.size() == 2, "cap still enforced");
    }

    // 17. persist/restore 往返保留 banned_until（时间盒跨重启）
    {
        auto file = (dir / "ban_state.json").string();
        uint64_t now = 3000;
        {
            pr::IntrusionDetector d(file);
            d.set_enabled(true);
            d.set_ban_threshold(1);
            d.set_ban_duration_sec(500);
            d.set_now_source_for_test([&now] { return now; });
            d.record_session("7.7.7.7", false);
            expect_true(d.is_banned("7.7.7.7"), "banned before persist");
            d.persist();
        }
        pr::IntrusionDetector d2(file);
        d2.set_enabled(true);
        d2.set_now_source_for_test([&now] { return now; });
        expect_true(d2.restore(), "restore ok");
        expect_true(d2.is_banned("7.7.7.7"), "time-boxed ban survives restart");
        now = 3600;
        expect_true(!d2.is_banned("7.7.7.7"), "restored ban expires by duration");
    }

    std::filesystem::remove_all(dir);
    std::printf("  pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
