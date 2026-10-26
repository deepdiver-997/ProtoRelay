#ifndef PR_FRAMEWORK_HTTP_HTTP_FSM_TPP
#define PR_FRAMEWORK_HTTP_HTTP_FSM_TPP
// 依赖：需以完整定义可见的方式包含，见 http_fsm_inst.cpp 的 include 顺序。
// 本文件在实例化前依赖 HttpSession<ConnectionType> 的完整定义（serve() 等）。
#include "framework/http/http_session.h"
#include "framework/http/http_session.tpp"
#include "framework/log/log_macros.h"

namespace pr::http {

template <typename ConnectionType>
void HttpFsm<ConnectionType>::init_engine() {
    // 表外的 (state,event) 一律 400 并关连接（承接旧 on_invalid_transition 与
    // WAIT_BODY/RESPOND 两个 fallback handler 的合并语义）。回复文本不由引擎
    // 写（desc 的 reply 留空，写空串是无操作），observer 独占拒绝路径。
    engine_.set_violation_observer(
        [](const Engine::SessionPtr& session) {
            auto* s = static_cast<HttpSession<ConnectionType>*>(session.get());
            s->send_error_and_close(400);
        });
    auto add = [this](HttpState st, HttpEvent ev,
                      void (HttpFsm::*handler)(std::shared_ptr<Session>)) {
        engine_.add_handler(st, ev, [this, handler](std::shared_ptr<Session> session) {
            (this->*handler)(std::move(session));
        });
    };
    add(HttpState::WAIT_REQUEST_LINE, HttpEvent::REQUEST_LINE, &HttpFsm::handle_request_line);
    add(HttpState::WAIT_HEADERS, HttpEvent::HEADER_LINE, &HttpFsm::handle_header_line);
    add(HttpState::WAIT_HEADERS, HttpEvent::HEADER_END, &HttpFsm::handle_header_end);
    add(HttpState::WAIT_BODY, HttpEvent::BODY, &HttpFsm::handle_body);
    add(HttpState::WAIT_BODY, HttpEvent::BODY_END, &HttpFsm::handle_body_end);
}

template <typename ConnectionType>
void HttpFsm<ConnectionType>::auto_process_event(
    std::shared_ptr<Session> session)
{
    engine_.auto_process_event(std::move(session));
}

template <typename ConnectionType>
void HttpFsm<ConnectionType>::handle_request_line(
    std::shared_ptr<Session> session)
{
    // 请求行已在 HttpSession::handle_read 里解析进 request_。apply 收口：
    // 落 WAIT_HEADERS + drain/续读（与外层读循环 auto_continue_read 的续消费
    // 等价；重复 do_async_read 有 in-flight 守卫，是无害 no-op）。
    engine_.apply(session, HttpState::WAIT_HEADERS);
}

template <typename ConnectionType>
void HttpFsm<ConnectionType>::handle_header_line(
    std::shared_ptr<Session> session)
{
    engine_.apply(session, HttpState::WAIT_HEADERS);
}

template <typename ConnectionType>
void HttpFsm<ConnectionType>::handle_header_end(
    std::shared_ptr<Session> session)
{
    auto* s = static_cast<HttpSession<ConnectionType>*>(session.get());
    if (s->has_body_pending()) {
        s->begin_body();                      // 武装 body 消费模式（extract 改判帧）
        engine_.apply(session, HttpState::WAIT_BODY);
    } else {
        // RESPOND 分支不 drain：流水线上的下一个请求必须等 finish_response
        // 复位到 WAIT_REQUEST_LINE 后再消费（提前 drain 会在 RESPOND 态把它
        // 误派发成 400）。apply_closing = 只落态，读/消费权仍在响应管线。
        engine_.apply_closing(session, HttpState::RESPOND);
        serve(session);
    }
}

template <typename ConnectionType>
void HttpFsm<ConnectionType>::handle_body(
    std::shared_ptr<Session> session)
{
    // 本段 body 已在 handle_read 消费；body 未收完，保持 WAIT_BODY。
    engine_.apply(session, HttpState::WAIT_BODY);
}

template <typename ConnectionType>
void HttpFsm<ConnectionType>::handle_body_end(
    std::shared_ptr<Session> session)
{
    engine_.apply_closing(session, HttpState::RESPOND);
    auto* s = static_cast<HttpSession<ConnectionType>*>(session.get());
    s->serve();
}

// 响应：转交 HttpSession（它持连接/写原语/请求上下文）。
template <typename ConnectionType>
void HttpFsm<ConnectionType>::serve(
    std::shared_ptr<Session> session)
{
    static_cast<HttpSession<ConnectionType>*>(session.get())->serve();
}

} // namespace pr::http

#endif // PR_FRAMEWORK_HTTP_HTTP_FSM_TPP
