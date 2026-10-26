#ifndef PR_FRAMEWORK_HTTP_HTTP_FSM_H
#define PR_FRAMEWORK_HTTP_HTTP_FSM_H
// ──────────────────────────────────────────────────────────────────
// HttpFsm — HTTP/1.1 状态机（pr::fsm::Engine 持有，fsm-extract 第 4 步）
//
// 状态图（keep-alive 闭环；docs/architecture/fsm 有三邮件协议的渲染先例）：
//
//   WAIT_REQUEST_LINE ──REQUEST_LINE──▶ WAIT_HEADERS
//   WAIT_HEADERS     ──HEADER_LINE──▶  WAIT_HEADERS
//   WAIT_HEADERS     ──HEADER_END──▶   WAIT_BODY    (有 body，handler 分流)
//   WAIT_HEADERS     ──HEADER_END──▶   RESPOND      (无 body，handler 分流)
//   WAIT_BODY        ──BODY──▶         WAIT_BODY
//   WAIT_BODY        ──BODY_END──▶     RESPOND
//   RESPOND          ──(finish_response 写回调)──▶ WAIT_REQUEST_LINE
//   CLOSED 为终态；表外的 (state,event) 一律 400 并关连接。
//
// handler 与三邮件协议同一签名：void(std::shared_ptr<pr::SessionBase<...>>)。
// "何时读"由 pr::SessionBase 的 do_async_read + 覆写的 extract_one_line 决定；
// "这一块怎么解析"在 HttpSession::handle_read；本类只做状态流转 + 触发响应。
// ──────────────────────────────────────────────────────────────────
#include "framework/session_base.h"
#include "framework/fsm/fsm_engine.h"
#include "framework/http/http_types.hpp"
#include <memory>

namespace pr::http {

template <typename ConnectionType> class HttpSession;   // 前向声明

// HTTP 协议描述：事件由帧解析产生，命令表为空；HEADER_END 的两个出口都是
// 真实写点（handler 按 body framing 分流，同 SMTP DATA_END 双行先例）。
// 非法输入的回复不由 desc 表达（400 是整段 HTTP 响应 + 关连接，由
// HttpFsm 在引擎上注册的 rejection observer 发出），error_policy.reply 留空。
inline fsm::Desc<HttpState, HttpEvent> make_http_desc() {
    fsm::Desc<HttpState, HttpEvent> d;
    // d.commands 空：HTTP 无文本命令词，事件全部来自 HttpSession 的帧解析。
    d.guards = {
        {HttpState::WAIT_REQUEST_LINE, HttpEvent::REQUEST_LINE, HttpState::WAIT_HEADERS},
        {HttpState::WAIT_HEADERS, HttpEvent::HEADER_LINE, HttpState::WAIT_HEADERS},
        // 同键两行 = HEADER_END 的两个真实写点（有 body / 无 body，handler
        // 决定走向；旧数组引擎"后写胜出"从未执行过这两行的 to）。合法性与
        // strict-guard 断言目标都成立。
        {HttpState::WAIT_HEADERS, HttpEvent::HEADER_END, HttpState::WAIT_BODY},
        {HttpState::WAIT_HEADERS, HttpEvent::HEADER_END, HttpState::RESPOND},
        {HttpState::WAIT_BODY, HttpEvent::BODY, HttpState::WAIT_BODY},
        {HttpState::WAIT_BODY, HttpEvent::BODY_END, HttpState::RESPOND},
    };
    d.error_policy = {"", 0};   // 拒绝回复由 observer 全权发出（400 + close）
    d.timeout_policy = {"", true};
    d.greeting.clear();         // HTTP 无 banner
    d.internal_error_reply.clear();  // 屏障只记日志（异常不再打穿 io run loop）
    d.terminals = {HttpState::CLOSED};
    d.state_names = {"WAIT_REQUEST_LINE", "WAIT_HEADERS", "WAIT_BODY", "RESPOND", "CLOSED"};
    d.event_names = {"REQUEST_LINE", "HEADER_LINE", "HEADER_END", "BODY",
                     "BODY_END", "ERROR", "TIMEOUT"};
    return d;
}

template <typename ConnectionType>
class HttpFsm {
public:
    using Engine = fsm::Engine<ConnectionType, HttpState, HttpEvent>;
    using Session = pr::SessionBase<ConnectionType>;

    explicit HttpFsm() : engine_(make_http_desc()) { init_engine(); }
    ~HttpFsm() = default;

    // Session::process_read 调用的公共入口：派发 next_event_
    void auto_process_event(std::shared_ptr<Session> session);

private:
    void init_engine();   // rejection observer（需 HttpSession 完整定义，在 .tpp）

    void handle_request_line(std::shared_ptr<Session> session);
    void handle_header_line(std::shared_ptr<Session> session);
    void handle_header_end(std::shared_ptr<Session> session);
    void handle_body(std::shared_ptr<Session> session);
    void handle_body_end(std::shared_ptr<Session> session);

    // 响应收尾：写响应并处理 keep-alive / 断开，供 header_end / body_end 共用。
    void serve(std::shared_ptr<Session> session);

    Engine engine_;

    friend class HttpSession<ConnectionType>;
};

} // namespace pr::http

#endif // PR_FRAMEWORK_HTTP_HTTP_FSM_H
