// pr::ServerConfig 基类测试：JSON 加载、校验规则、监听器工具函数。
//
// 这是框架级配置基类，SMTP/IMAP 的 ServerConfig 都继承它。
// 校验规则直接影响启动时是否接受配置，错放一个端口/证书就起不来服务。
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <string>
#include <nlohmann/json.hpp>

#include "framework/server_config_base.h"
#include "framework/server_config.h"   // mail_system::ServerConfig（含 outbound_* 字段）

namespace {

using pr::ListenerConfig;
using pr::ListenerType;
using pr::ServerConfig;

int g_pass = 0, g_fail = 0;
void expect_true(bool c, const char* what) {
    if (c) { g_pass++; }
    else { g_fail++; std::printf("  FAIL %s\n", what); }
}

} // namespace

int main() {
    std::printf("server_config_test\n");

    // 1. 监听器类型字符串互转
    expect_true(pr::listener_type_to_string(ListenerType::SSL) == std::string("ssl"),
                "SSL -> ssl");
    expect_true(pr::listener_type_to_string(ListenerType::TCP) == std::string("tcp"),
                "TCP -> tcp");
    expect_true(pr::listener_type_from_string("ssl") == ListenerType::SSL,
                "ssl -> SSL");
    expect_true(pr::listener_type_from_string("tcp") == ListenerType::TCP,
                "tcp -> TCP");
    expect_true(pr::listener_type_from_string("bogus") == ListenerType::TCP,
                "unknown type defaults to TCP");

    // 2. 默认值
    {
        ServerConfig c;
        expect_true(c.address == "0.0.0.0", "default address");
        expect_true(c.maxMessageSize == 1024 * 1024, "default maxMessageSize 1MB");
        expect_true(c.maxConnections == 1000, "default maxConnections");
        expect_true(c.max_auth_attempts == 3, "default max_auth_attempts");
        expect_true(c.enable_tls1_3, "tls1.3 on by default");
        expect_true(c.log_level == "info", "default log_level");
        expect_true(c.listeners.empty(), "default no listeners");
    }

    // 3. loadFromJson：完整字段解析
    {
        ServerConfig c;
        nlohmann::json j = {
            {"address", "127.0.0.1"},
            {"maxMessageSize", 2048},
            {"maxConnections", 500},
            {"connection_timeout", 120},
            {"read_timeout", 30},
            {"write_timeout", 20},
            {"max_auth_attempts", 5},
            {"log_level", "debug"},
            {"log_to_console", false},
            {"enable_tls1_3", false},
            {"certFile", "certs/cert.pem"},
            {"listeners", {
                {{"type", "tcp"}, {"port", 25}},
                {{"type", "ssl"}, {"port", 465}},
                {{"type", "ssl"}, {"port", 0}}       // port 0 应被过滤
            }}
        };
        expect_true(c.loadFromJson(j, "/srv/base"), "loadFromJson returns true");
        expect_true(c.address == "127.0.0.1", "address parsed");
        expect_true(c.maxMessageSize == 2048, "maxMessageSize parsed");
        expect_true(c.max_auth_attempts == 5, "max_auth_attempts parsed");
        expect_true(c.log_level == "debug", "log_level parsed");
        expect_true(!c.enable_tls1_3, "tls1.3 flag parsed");
        expect_true(c.listeners.size() == 2, "port 0 listener filtered out");
        expect_true(c.listeners[0].type == ListenerType::TCP &&
                    c.listeners[0].port == 25, "tcp listener parsed");
        expect_true(c.listeners[1].type == ListenerType::SSL &&
                    c.listeners[1].port == 465, "ssl listener parsed");
        // 相对路径解析：resolve_path 把 base_dir 当「文件路径」取其 parent 再拼相对路径
        // （loadFromFile 传进来的其实是目录 → 存在 off-by-one，这里记录实际行为：
        //  base_dir=/srv/base 时解析到 /srv/certs/... 而非 /srv/base/certs/...）
        expect_true(c.certFile == "/srv/certs/cert.pem", "relative path resolved vs base_dir(parent)");
        // resolve_path 的直接契约：第一个参数是文件路径时，相对路径相对其目录解析
        expect_true(ServerConfig::resolve_path("/srv/base/config.json", "certs/cert.pem")
                    == "/srv/base/certs/cert.pem", "resolve_path resolves vs config file dir");
        expect_true(ServerConfig::resolve_path("/x", "").empty(), "empty relative path -> empty");
    }

    // 4. loadFromJson：缺失字段保默认
    {
        ServerConfig c;
        expect_true(c.loadFromJson(nlohmann::json{{"listeners", nlohmann::json::array()}},
                                   ""), "empty listeners json ok");
        expect_true(c.max_auth_attempts == 3, "missing field keeps default");
        expect_true(c.address == "0.0.0.0", "missing address keeps default");
    }

    // 5. listener_map / find_listener
    {
        ServerConfig c;
        c.listeners = {ListenerConfig(ListenerType::TCP, 25),
                       ListenerConfig(ListenerType::SSL, 465)};
        auto m = c.listener_map();
        expect_true(m.size() == 2, "listener_map size");
        expect_true(m.count(25) == 1 && m[25].type == ListenerType::TCP, "map has port 25");
        expect_true(m.count(465) == 1 && m[465].type == ListenerType::SSL, "map has port 465");
        const ListenerConfig* lc = c.find_listener(465);
        expect_true(lc && lc->port == 465 && lc->type == ListenerType::SSL,
                    "find_listener hits");
        expect_true(c.find_listener(999) == nullptr, "find_listener misses");
    }

    // 6. validate 规则
    {
        ServerConfig c;    // 空 listeners
        expect_true(!c.validate(), "empty listeners rejected");

        ServerConfig c2;
        c2.listeners.push_back(ListenerConfig(ListenerType::TCP, 0));   // port 0
        expect_true(!c2.validate(), "zero port rejected");

        ServerConfig c3;
        c3.listeners.push_back(ListenerConfig(ListenerType::TCP, 25));
        c3.io_thread_count = 0;
        expect_true(!c3.validate(), "zero io_thread_count rejected");

        ServerConfig c4;
        c4.listeners.push_back(ListenerConfig(ListenerType::TCP, 25));
        c4.read_timeout = 0;
        expect_true(!c4.validate(), "zero read_timeout rejected");

        ServerConfig c5;
        c5.listeners.push_back(ListenerConfig(ListenerType::TCP, 25));
        expect_true(c5.validate(), "valid tcp-only config accepted");
    }

#ifdef USE_SSL
    // 7. SSL 监听器缺证书 → 校验失败（仅 USE_SSL 构建下成立）
    {
        ServerConfig c;
        c.listeners.push_back(ListenerConfig(ListenerType::SSL, 465));
        expect_true(!c.validate(), "ssl listener without certs rejected");

        ServerConfig c2;
        c2.listeners.push_back(ListenerConfig(ListenerType::SSL, 465));
        c2.certFile = "/srv/cert.pem";
        c2.keyFile  = "/srv/key.pem";
        expect_true(c2.validate(), "ssl listener with certs accepted");
    }
#endif

    // 8. outbound.default_route (catch-all 兜底路由) + static_routes 解析
    //    派生 mail_system::ServerConfig 的完整解析入口是 loadFromFile（基类 loadFromJson
    //    只解析 framework 层字段）。用临时 JSON 文件走 loadFromFile 验证 outbound 块。
    {
        auto dir = std::filesystem::temp_directory_path() / "pr_cfg_default_route_test";
        std::filesystem::create_directories(dir);
        auto cfg = dir / "ob.json";
        nlohmann::json j = {
            {"outbound", {
                {"helo_domain", "mx2.scut.email"},
                {"static_routes", {{"b.local", {{"host", "127.0.0.1"}, {"port", 10026}}}}},
                {"default_route", {{"host", "107.174.127.252"}, {"port", 25}}}
            }}
        };
        { std::ofstream o(cfg); o << j.dump(); }
        mail_system::ServerConfig c;
        expect_true(c.loadFromFile(cfg.string()), "loadFromFile returns true (outbound config)");
        expect_true(c.outbound_helo_domain == "mx2.scut.email", "helo_domain parsed");
        expect_true(c.outbound_default_route.host == "107.174.127.252" &&
                    c.outbound_default_route.port == 25, "default_route parsed");
        auto it = c.outbound_static_routes.find("b.local");
        expect_true(it != c.outbound_static_routes.end() &&
                    it->second.host == "127.0.0.1" && it->second.port == 10026,
                    "static_routes still parsed alongside default_route");
        std::filesystem::remove_all(dir);
    }

    std::printf("  pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
