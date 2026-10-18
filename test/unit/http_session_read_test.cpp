// 2026-10-18：HTTP 请求/headers/body 分片时停读，写信 POST 挂至 watchdog。
// docs/bugfixes/2026-10-18-http-fragmented-request-read.md
// 模拟真实 transport 分片：一段只交给一个 pending read，验证下一次 read 会被
// 发起，以及消费者最后拿到完整 body。不能直接喂 handle_read（会绕过事故路径）。
#undef NDEBUG
#include <cassert>
#include "framework/http/http_session.h"
#include "framework/session_base.tpp"
#include "framework/http/http_fsm.tpp"
#include "mock_connection.h"
#include <iostream>

using Http = pr::http::HttpSession<mail_system::MockConnection>;
using Fsm = pr::http::HttpFsm<mail_system::MockConnection>;

static void fragmented_request(const std::vector<std::string>& chunks) {
    auto connection = std::make_unique<mail_system::MockConnection>();
    auto* conn = connection.get();
    conn->set_deferred_read(true);
    auto session = std::make_shared<Http>(nullptr, std::move(connection), std::make_shared<Fsm>());
    int calls = 0;
    std::function<void(pr::http::HttpResponse)> complete;
    session->set_api_handler([&](const pr::http::HttpRequest& request, auto cb) {
        ++calls;
        assert(request.path == "/api/echo" && request.body == "abcdefg");
        complete = std::move(cb);
        return true;
    });
    Http::start(session);
    conn->pump_executor();
    for (size_t i = 0; i < chunks.size(); ++i) {
        assert(conn->has_pending_read());
        conn->trigger_deferred_read(chunks[i]);
        conn->pump_executor();
        if (i + 1 != chunks.size()) {
            assert(calls == 0);
            assert(conn->has_pending_read()); // 修复前在第一个残缺请求行即 FAIL
        }
    }
    assert(calls == 1 && complete);
    assert(!conn->has_pending_read()); // API 在异步工作时保持 pause，不重复读
    pr::http::HttpResponse response;
    response.body = "complete-body";
    complete(std::move(response));
    conn->pump_executor();
    assert(conn->written().find("HTTP/1.1 200 OK\r\n") == 0);
    assert(conn->written().find("complete-body") != std::string::npos);
    assert(session->is_closed());
    complete = {};
}

int main() {
    fragmented_request({"PO", "ST /api/echo HTTP/1.1\r\nHost: loc", "al\r\nConnection: close\r\nContent-Length: 7\r\n\r\n", "abc", "defg"});
    fragmented_request({"POST /api/echo HTTP/1.1\r\nHost: local\r\nConnection: close\r\nTransfer-Encoding: chunked\r\n\r\n",
                        "7\r", "\nabc", "def", "g\r", "\n0\r", "\n", "\r", "\n"});
    std::cout << "PASS: fragmented HTTP request line, headers, fixed body, chunked body, async API pause\n";
}
