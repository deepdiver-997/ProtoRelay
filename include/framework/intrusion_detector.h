#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <fstream>
#include <chrono>
#include <atomic>
#include <functional>
#include <algorithm>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace pr {

struct IpRecord {
    int      failed_count = 0;
    int      total_connections = 0;
    uint64_t first_seen = 0;
    uint64_t last_seen  = 0;
    uint64_t window_start = 0;   // 失败计数窗口起点（fail_window_sec>0 时有效）
    uint64_t banned_until = 0;   // 时间盒封禁截止（0=未时间盒封禁）

    nlohmann::json to_json() const {
        return {
            {"failed_count", failed_count},
            {"total_connections", total_connections},
            {"first_seen", first_seen},
            {"last_seen", last_seen},
            {"window_start", window_start},
            {"banned_until", banned_until}
        };
    }

    static IpRecord from_json(const nlohmann::json& j) {
        IpRecord r;
        r.failed_count      = j.value("failed_count", 0);
        r.total_connections = j.value("total_connections", 0);
        r.first_seen        = j.value("first_seen", 0ULL);
        r.last_seen         = j.value("last_seen", 0ULL);
        r.window_start      = j.value("window_start", 0ULL);
        r.banned_until      = j.value("banned_until", 0ULL);
        return r;
    }
};

// IP 匹配器：精确串 + IPv4 CIDR（a.b.c.d/n）。IPv6 只支持精确匹配。
struct IpMatcher {
    std::unordered_set<std::string> exact;
    std::vector<std::pair<uint32_t, uint32_t>> cidrs;  // (net, mask), 主机序

    void add(const std::string& entry) {
        auto slash = entry.find('/');
        if (slash == std::string::npos) { exact.insert(entry); return; }
        uint32_t net = parse_ipv4(entry.substr(0, slash));
        if (net == 0xFFFFFFFFu && entry.substr(0, slash) != "255.255.255.255") return;
        int bits = 0;
        try { bits = std::stoi(entry.substr(slash + 1)); } catch (...) { return; }
        if (bits < 0 || bits > 32) return;
        uint32_t mask = bits == 0 ? 0u : 0xFFFFFFFFu << (32 - bits);
        cidrs.emplace_back(net & mask, mask);
    }

    bool match(const std::string& ip) const {
        if (exact.count(ip)) return true;
        uint32_t v = parse_ipv4(ip);
        if (v == 0xFFFFFFFFu && ip != "255.255.255.255") return false;
        for (auto& [net, mask] : cidrs)
            if ((v & mask) == net) return true;
        return false;
    }

    bool empty() const { return exact.empty() && cidrs.empty(); }

private:
    // 非法输入返回哨兵值 0xFFFFFFFF（与真实 255.255.255.255 区分靠调用方的字符串比对）
    static uint32_t parse_ipv4(const std::string& s) {
        uint32_t out = 0;
        int octets = 0;
        size_t i = 0;
        while (i < s.size() && octets < 4) {
            uint32_t val = 0;
            int digits = 0;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
                val = val * 10 + (s[i] - '0');
                if (val > 255) return 0xFFFFFFFFu;
                ++i; ++digits;
            }
            if (digits == 0 || digits > 3) return 0xFFFFFFFFu;
            out = (out << 8) | val;
            ++octets;
            if (octets < 4) {
                if (i >= s.size() || s[i] != '.') return 0xFFFFFFFFu;
                ++i;
            }
        }
        if (octets != 4 || i != s.size()) return 0xFFFFFFFFu;
        return out;
    }
};

class IntrusionDetector {
public:
    explicit IntrusionDetector(std::string data_file = "logs/intrusion_data.json")
        : m_data_file(std::move(data_file)), m_enabled(false),
          m_max_records(10000), m_ban_threshold(0),
          m_fail_window_sec(0), m_ban_duration_sec(0),
          m_persist_interval_sec(60), m_persist_dirty_threshold(256),
          m_records_since_persist(0), m_last_persist_time(0) {}

    void set_enabled(bool enabled) { m_enabled.store(enabled); }
    bool is_enabled() const { return m_enabled.load(); }
    void set_max_records(size_t n) { m_max_records = n; }
    void set_ban_threshold(int n) { m_ban_threshold = n; }
    // 失败计数窗口：>0 时窗口过期后 failed_count 清零重新累计；0=不重置（旧行为）。
    void set_fail_window_sec(int sec) { std::lock_guard<std::mutex> lock(m_mutex); m_fail_window_sec = sec; }
    // 封禁时长：>0 时达标后进入时间盒封禁，到期自动解封；0=永久（旧行为）。
    void set_ban_duration_sec(int sec) { std::lock_guard<std::mutex> lock(m_mutex); m_ban_duration_sec = sec; }
    void set_persist_interval(int sec) { m_persist_interval_sec = sec; }
    void set_persist_dirty_threshold(int n) { m_persist_dirty_threshold = n; }
    void set_data_file(std::string f) { std::lock_guard<std::mutex> lock(m_mutex); m_data_file = std::move(f); }
    void set_now_source_for_test(std::function<uint64_t()> fn) { std::lock_guard<std::mutex> lock(m_mutex); m_now = std::move(fn); }

    // 白名单：不记账、永不封禁。黑名单：无条件拒绝，优先级高于白名单。
    void set_whitelist(const std::vector<std::string>& entries) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_whitelist = IpMatcher{};
        for (auto& e : entries) m_whitelist.add(e);
    }
    // 黑名单文件：每行一个 IP 或 IPv4 CIDR，'#' 注释。设置即加载，此后
    // is_banned 每次节流检查 mtime，文件被外部改动自动重载（多进程共享
    // 同一份黑名单文件的机制）。
    void set_blacklist_file(const std::string& path) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_blacklist_file = path;
        m_bl_mtime = 0;
        m_bl_last_check = 0;
        reload_blacklist_locked();
    }
    size_t blacklist_size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_blacklist.exact.size() + m_blacklist.cidrs.size();
    }

    // 每个会话结束时调用。返回"本次记录是否触发封禁"（ crossing 阈值的那一次）。
    bool record_session(const std::string& ip, bool authenticated) {
        if (!m_enabled.load() || ip.empty()) return false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_whitelist.match(ip)) return false;
        }
        if (is_private_ip(ip)) return false;

        uint64_t now = m_now();
        bool just_banned = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto& rec = m_records[ip];
            rec.total_connections++;
            if (!authenticated) {
                if (m_fail_window_sec > 0) {
                    if (rec.window_start == 0) rec.window_start = now;
                    if (now - rec.window_start > static_cast<uint64_t>(m_fail_window_sec)) {
                        rec.failed_count = 0;
                        rec.window_start = now;
                        if (rec.banned_until != 0 && rec.banned_until <= now) rec.banned_until = 0;
                    }
                }
                rec.failed_count++;
                if (m_ban_threshold > 0 && rec.failed_count == m_ban_threshold) {
                    if (m_ban_duration_sec > 0 && rec.banned_until == 0) {
                        rec.banned_until = now + static_cast<uint64_t>(m_ban_duration_sec);
                        m_records_since_persist = m_persist_dirty_threshold;  // 封禁立即落盘
                    }
                    just_banned = true;
                }
            }
            if (rec.first_seen == 0) rec.first_seen = now;
            rec.last_seen = now;

            // LRU eviction（在封期内的时间盒记录不参与驱逐，防扫描器洗掉封禁）
            while (m_records.size() > m_max_records) {
                auto oldest = m_records.end();
                for (auto it = m_records.begin(); it != m_records.end(); ++it) {
                    if (it->second.banned_until > now) continue;
                    if (oldest == m_records.end() ||
                        it->second.last_seen < oldest->second.last_seen)
                        oldest = it;
                }
                if (oldest == m_records.end()) break;  // 全部在封期内
                m_records.erase(oldest);
            }

            // 懒刷盘：记录数超过脏阈值时才检查时间，避免频繁系统调用
            m_records_since_persist++;
            if (m_records_since_persist >= m_persist_dirty_threshold) {
                if (m_last_persist_time == 0 ||
                    now - m_last_persist_time >= static_cast<uint64_t>(m_persist_interval_sec)) {
                    persist_locked();
                    m_records_since_persist = 0;
                    m_last_persist_time = now;
                }
            }
        }
        return just_banned;
    }

    // 判断 IP 是否应被拒绝服务
    bool is_banned(const std::string& ip) const {
        maybe_reload_blacklist();
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_blacklist.empty() && m_blacklist.match(ip)) return true;
        if (!m_whitelist.empty() && m_whitelist.match(ip)) return false;
        auto it = m_records.find(ip);
        if (it == m_records.end()) return false;
        uint64_t now = m_now();
        if (it->second.banned_until != 0) return now < it->second.banned_until;
        // 旧行为（ban_duration=0）：动态判定，阈值提高可追溯封禁既有计数
        return m_ban_duration_sec == 0 && m_ban_threshold > 0 &&
               it->second.failed_count >= m_ban_threshold;
    }

    // 查询某个 IP 的记录
    IpRecord query(const std::string& ip) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_records.find(ip);
        if (it != m_records.end()) return it->second;
        return IpRecord{};
    }

    // 获取完整快照
    std::unordered_map<std::string, IpRecord> snapshot() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_records;
    }

    // 持久化到磁盘（显式调用，如 shutdown 时）
    bool persist() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return persist_locked();
    }

    // 从磁盘恢复
    bool restore() {
        if (m_data_file.empty()) return false;
        try {
            std::ifstream ifs(m_data_file);
            if (!ifs) return false;
            nlohmann::json j = nlohmann::json::parse(ifs);
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto it = j.begin(); it != j.end(); ++it) {
                m_records[it.key()] = IpRecord::from_json(it.value());
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_records.size();
    }

private:
    static bool is_private_ip(const std::string& ip) {
        if (ip == "127.0.0.1" || ip == "::1" || ip == "0.0.0.0") return true;
        if (ip.compare(0, 3, "10.") == 0) return true;
        if (ip.compare(0, 8, "192.168.") == 0) return true;
        if (ip.compare(0, 4, "172.") == 0) {
            auto pos = ip.find('.', 4);
            if (pos != std::string::npos) {
                int second = std::stoi(ip.substr(4, pos - 4));
                if (second >= 16 && second <= 31) return true;
            }
        }
        return false;
    }

    // 黑名单 mtime 节流检查（is_banned 热路径上最多每 10 秒一次 stat）
    void maybe_reload_blacklist() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_blacklist_file.empty()) return;
        uint64_t now = m_now();
        if (m_bl_last_check != 0 && now - m_bl_last_check < 10) return;
        m_bl_last_check = now;
        try {
            auto t = std::filesystem::last_write_time(std::filesystem::path(m_blacklist_file))
                         .time_since_epoch().count();
            if (t != m_bl_mtime) {
                m_bl_mtime = t;
                reload_blacklist_locked();
            }
        } catch (...) {
            // 文件暂时不可 stat（被替换中）：保留旧表
        }
    }

    // 必须在持锁状态下调用
    void reload_blacklist_locked() const {
        m_blacklist = IpMatcher{};
        if (m_blacklist_file.empty()) return;
        std::ifstream ifs(m_blacklist_file);
        if (!ifs) return;
        std::string line;
        while (std::getline(ifs, line)) {
            auto hash = line.find('#');
            if (hash != std::string::npos) line = line.substr(0, hash);
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t' ||
                                     line.back() == '\r' || line.back() == '\n'))
                line.pop_back();
            size_t b = 0;
            while (b < line.size() && (line[b] == ' ' || line[b] == '\t')) ++b;
            if (b >= line.size()) continue;
            m_blacklist.add(line.substr(b));
        }
    }

    static uint64_t now_sec() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
    }

private:
    // 必须在持锁状态下调用
    bool persist_locked() const {
        if (m_data_file.empty()) return false;
        nlohmann::json j;
        for (const auto& [ip, rec] : m_records)
            j[ip] = rec.to_json();
        try {
            std::error_code ec;
            std::filesystem::path p(m_data_file);
            if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
            std::ofstream ofs(m_data_file, std::ios::trunc);
            if (!ofs) return false;
            ofs << j.dump(2);
            return true;
        } catch (...) {
            return false;
        }
    }

    std::string m_data_file;
    std::atomic<bool> m_enabled;
    size_t m_max_records;
    int    m_ban_threshold;
    int    m_fail_window_sec;
    int    m_ban_duration_sec;
    int    m_persist_interval_sec;
    int    m_persist_dirty_threshold;
    int    m_records_since_persist;
    uint64_t m_last_persist_time;
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, IpRecord> m_records;
    std::function<uint64_t()> m_now = now_sec;
    IpMatcher m_whitelist;
    mutable IpMatcher m_blacklist;  // is_banned 为 const，文件重载在锁内改写
    std::string m_blacklist_file;
    // 文件黑名单的重载节流状态（is_banned 为 const，需 mutable）
    mutable int64_t m_bl_mtime = 0;
    mutable uint64_t m_bl_last_check = 0;
};

} // namespace pr
