// STARTTLS handoff 回归测试 —— 2026-09-25 生产问题：
// handoff_starttls_socket 给升级出的 TLS 会话传 ListenerConfig{} 默认值，
// 587 提交端口 STARTTLS 后 auth_policy 退化成 OFF，EHLO 不再通告 AUTH，
// 客户端无法认证（465 原生 TLS 会话不受影响）。MAIL FROM 的 require_auth
// 判定同样失真。修复：按 socket 本地端口从 m_listener_configs 反查真实
// 监听器配置。本测试用 recording 会话工厂锁定「handoff 传入的 lc == 监听器配置」。
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <memory>
#include <string>

#include <boost/asio.hpp>

#include "framework/tcp_server_base.h"

using namespace mail_system;

namespace {

int g_pass = 0, g_fail = 0;
void expect_true(bool c, const char* what) {
    if (c) { g_pass++; }
    else { g_fail++; std::printf("  FAIL %s\n", what); }
}

// 最小 TcpSession 桩：补齐模板实例化（start/accept 路径）所需的成员
struct FakeTcpSession {
    void set_tracks_connection_count(bool) {}
    static void start(std::shared_ptr<FakeTcpSession>) {}
};

// 记录 make_ssl_session 收到的 ListenerConfig
struct RecordingSslSession {
    void set_tracks_connection_count(bool) {}
    void set_trace_buffer(std::string) {}
    static void start(std::shared_ptr<RecordingSslSession>) {}
    static void start_after_starttls(std::shared_ptr<RecordingSslSession>) {}
};

struct HandoffTestServer : TcpServerBase<FakeTcpSession, RecordingSslSession> {
    using Base = TcpServerBase<FakeTcpSession, RecordingSslSession>;
    using Base::Base;
    bool ssl_session_created = false;
    ListenerConfig received_lc{};
    bool should_reject_connection(std::string& reason, const std::string&) const override {
        reason.clear();
        return false;
    }
    std::shared_ptr<FakeTcpSession> make_tcp_session(
        std::unique_ptr<TcpConnection>, const ListenerConfig&) override {
        return nullptr;
    }
    std::shared_ptr<RecordingSslSession> make_ssl_session(
        std::unique_ptr<SslConnection>, const ListenerConfig& lc) override {
        ssl_session_created = true;
        received_lc = lc;
        return std::make_shared<RecordingSslSession>();
    }
};

ServerConfig make_cfg() {
    ServerConfig cfg;
    cfg.use_database                = false;
    cfg.storage.provider            = "null";
    cfg.io_thread_count             = 0;
    cfg.worker_thread_count         = 0;
    cfg.metrics_enabled             = false;
    cfg.system_domain               = "test.local";
    cfg.intrusion_detection_enabled = false;
    cfg.log_level                   = "off";
    cfg.log_to_file                 = false;
    cfg.address                     = "127.0.0.1";
    // 不配 listeners：避开 ctor 绑端口；测试自建 acceptor 造本地端口已知的 socket
    return cfg;
}

} // namespace

int main() {
    std::printf("starttls_handoff_test\n");
    boost::asio::io_context io;
    using boost::asio::ip::tcp;

    // 用例 1：端口在 m_listener_configs 里 → 完整透传监听器配置
    {
        HandoffTestServer server(make_cfg());
        tcp::acceptor acc(io, tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
        uint16_t port = acc.local_endpoint().port();

        tcp::socket client(io);
        client.connect(acc.local_endpoint());
        tcp::socket accepted(io);
        acc.accept(accepted);
        acc.close();

        ListenerConfig lc(ListenerType::TCP, port);
        lc.auth_policy = InboundAuthPolicy::ON;   // 模拟 587 提交端口
        server.m_listener_configs[port] = lc;

        server.handoff_starttls_socket(
            std::make_unique<tcp::socket>(std::move(accepted)), "");
        expect_true(server.ssl_session_created, "handoff creates ssl session");
        expect_true(server.received_lc.auth_policy == InboundAuthPolicy::ON,
                    "handoff inherits listener auth_policy (587 AUTH regression)");
        expect_true(server.received_lc.port == port,
                    "handoff inherits listener port");
    }

    // 用例 2：端口不在表里（如测试外接）→ 安全回退默认配置，不崩
    {
        HandoffTestServer server(make_cfg());
        tcp::acceptor acc(io, tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
        (void)acc.local_endpoint().port();

        tcp::socket client(io);
        client.connect(acc.local_endpoint());
        tcp::socket accepted(io);
        acc.accept(accepted);
        acc.close();

        server.handoff_starttls_socket(
            std::make_unique<tcp::socket>(std::move(accepted)), "");
        expect_true(server.ssl_session_created,
                    "unknown-port handoff still creates session");
        expect_true(server.received_lc.auth_policy == InboundAuthPolicy::OFF &&
                        server.received_lc.port == 0,
                    "unknown-port handoff falls back to default config");
    }

    std::printf("  pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
