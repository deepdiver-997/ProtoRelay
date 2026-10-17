// pipeline_deferred_test —— 回归：IMAP 式 pause + worker 延迟完成级联丢响应
//
// 2026-09-27 生产事故（mx1:993 外部 100% 复现）：客户端把 LOGIN/SELECT/
// UID SEARCH 放进一个 TCP 段 pipelining，服务端全部处理成功（日志含
// login successful / UID subcmd done），但响应永远不出 wire，客户端超时
// 退出。单命令逐条发（真实监控脚本）完全正常。
//
// 本测试在 pr::SessionBase 层复刻该模式：每条命令 handle_read 时 pause，
// 测试扮演 worker 在"完成"时 do_async_write(响应) + drain_buffered_commands()
// —— 与 traditional_imaps_fsm.tpp 的 handle_login/handle_select/handle_search
// 完成路径相同的调用序列。断言：全部响应最终出现在 wire 上。
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>

#include "test_session.h"

namespace mail_system {

// IMAP 式延迟完成会话：LOGIN/SELECT/SEARCH 在 handle_read 里 pause（响应由
// 测试驱动的"worker 完成"补发），LOGOUT 立即响应并关连接。
class DeferrableSession : public pr::SessionBase<MockConnection> {
public:
    explicit DeferrableSession(std::unique_ptr<MockConnection> conn, pr::ServerBase* server = nullptr)
        : pr::SessionBase<MockConnection>(std::move(conn), server) {}

    std::vector<std::string> read_lines;

    std::chrono::milliseconds compute_reply_delay() const override { return {}; }
    bool has_buffered_input() const override {
        return command_read_buffer_.find("\r\n") != std::string::npos;
    }
    std::string extract_one_line() override {
        auto pos = command_read_buffer_.find("\r\n");
        if (pos == std::string::npos) return {};
        std::string line = command_read_buffer_.substr(0, pos + 2);
        command_read_buffer_.erase(0, pos + 2);
        return line;
    }
    void handle_read(const std::string& line) override {
        read_lines.push_back(line);
        if (line.find("LOGIN") != std::string::npos ||
            line.find("SELECT") != std::string::npos ||
            line.find("SEARCH") != std::string::npos) {
            set_paused(true);   // 响应由 worker 完成回调补发（本测试手动驱动）
        } else if (line.find("LOGOUT") != std::string::npos) {
            do_async_write("* BYE\r\nLOGOUT OK\r\n",
                [](std::shared_ptr<pr::SessionBase<MockConnection>> s,
                   const boost::system::error_code& ec) {
                    if (!ec) s->close();
                });
        }
    }
    void process_read() override {}
    void set_current_state(int) override {}
    void set_next_event(int) override {}
    int  get_current_state() const override { return 0; }
    int  get_next_event() const override { return 0; }
    void* get_fsm() const override { return nullptr; }
    void* get_context() override { return nullptr; }
    std::string get_last_command_args() const override { return {}; }
};

} // namespace mail_system

namespace {

using mail_system::DeferrableSession;
using mail_system::MockConnection;

int g_pass = 0, g_fail = 0;
void expect_true(bool c, const char* what) {
    if (c) { g_pass++; }
    else { g_fail++; std::printf("  FAIL %s\n", what); }
}

// 模拟 worker 完成：响应 + drain（与 IMAP FSM 完成回调相同的调用序列）
void worker_complete(const std::shared_ptr<DeferrableSession>& s, const char* resp) {
    s->do_async_write(resp,
        [](std::shared_ptr<pr::SessionBase<MockConnection>> s2,
           const boost::system::error_code& ec) {
            if (!ec) s2->do_async_read();
        });
    s->drain_buffered_commands();
    s->get_connection().pump_executor();
}

} // namespace

int main() {
    std::printf("pipeline_deferred_test\n");

    // 场景 K（生产静默形态）：LOGIN/SELECT/SEARCH 三连 pipelining，SEARCH 最后
    {
        auto s = std::make_shared<DeferrableSession>(std::make_unique<MockConnection>());
        s->get_connection().set_read_data("1 LOGIN\r\n2 SELECT\r\n3 SEARCH\r\n");
        s->do_async_read();
        s->get_connection().pump_executor();
        expect_true(s->read_lines.size() == 1, "K: only LOGIN consumed before pause");

        worker_complete(s, "R1 OK LOGIN\r\n");   // login 完成 → drain 处理 SELECT → pause
        worker_complete(s, "R2 OK SELECT\r\n");  // select 完成 → drain 处理 SEARCH → pause
        worker_complete(s, "R3 OK SEARCH\r\n");  // search 完成 → buffer 空 → 必须 flush

        const std::string written = s->get_connection().written();
        expect_true(written.find("R1") != std::string::npos, "K: R1 (LOGIN) reached wire");
        expect_true(written.find("R2") != std::string::npos, "K: R2 (SELECT) reached wire");
        expect_true(written.find("R3") != std::string::npos, "K: R3 (SEARCH) reached wire");
        mail_system::finish_session(s);
    }

    // 场景 E（生产静默形态 + LOGOUT 收尾）
    {
        auto s = std::make_shared<DeferrableSession>(std::make_unique<MockConnection>());
        s->get_connection().set_read_data(
            "1 LOGIN\r\n2 SELECT\r\n3 SEARCH\r\n4 LOGOUT\r\n");
        s->do_async_read();
        s->get_connection().pump_executor();

        worker_complete(s, "R1 OK LOGIN\r\n");
        worker_complete(s, "R2 OK SELECT\r\n");
        worker_complete(s, "R3 OK SEARCH\r\n");   // drain 处理 LOGOUT → BYE+OK → flush+close

        const std::string written = s->get_connection().written();
        expect_true(written.find("R1") != std::string::npos, "E: R1 reached wire");
        expect_true(written.find("R2") != std::string::npos, "E: R2 reached wire");
        expect_true(written.find("R3") != std::string::npos, "E: R3 reached wire");
        expect_true(written.find("LOGOUT OK") != std::string::npos, "E: LOGOUT reply reached wire");
        mail_system::finish_session(s);
    }

    // 对照组 M：LOGIN/SELECT/LOGOUT（生产实测此形态正常）
    {
        auto s = std::make_shared<DeferrableSession>(std::make_unique<MockConnection>());
        s->get_connection().set_read_data("1 LOGIN\r\n2 SELECT\r\n3 LOGOUT\r\n");
        s->do_async_read();
        s->get_connection().pump_executor();

        worker_complete(s, "R1 OK LOGIN\r\n");
        worker_complete(s, "R2 OK SELECT\r\n");   // drain 处理 LOGOUT

        const std::string written = s->get_connection().written();
        expect_true(written.find("R1") != std::string::npos, "M: R1 reached wire");
        expect_true(written.find("R2") != std::string::npos, "M: R2 reached wire");
        expect_true(written.find("LOGOUT OK") != std::string::npos, "M: LOGOUT reply reached wire");
        mail_system::finish_session(s);
    }

    std::printf("  pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
