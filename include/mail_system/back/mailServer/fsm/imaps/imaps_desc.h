#ifndef IMAPS_DESC_H
#define IMAPS_DESC_H
// IMAP protocol description for the fsm engine — pure data lifted verbatim
// from the retired FsmBase tables (traditional_imaps_fsm.tpp init_transition_
// table / hook overrides at main 18064f5). The former `if constexpr` on the
// connection type becomes the allow_starttls parameter. Tagged BAD/NO replies
// need the session's context tag, so they are NOT desc data — the FSM object
// registers the engine's session-aware reply formatter instead (design §3.4
// boundary: a desc builds without sessions). Only std types + the protocol's
// own enums.
#include "framework/fsm/fsm_desc.h"
#include "mail_system/back/mailServer/fsm/imaps/imap_types.hpp"

namespace mail_system {

inline pr::fsm::Desc<ImapState, ImapEvent> make_imaps_desc(bool allow_starttls) {
    pr::fsm::Desc<ImapState, ImapEvent> d;
    // First-token table (session parse maps commands itself; the table is
    // the declarative mirror of imaps_session.tpp get_imap_command_map).
    d.commands = {
        {"CAPABILITY", ImapEvent::CAPABILITY}, {"LOGIN", ImapEvent::LOGIN},
        {"AUTHENTICATE", ImapEvent::AUTHENTICATE}, {"LOGOUT", ImapEvent::LOGOUT},
        {"SELECT", ImapEvent::SELECT},         {"EXAMINE", ImapEvent::EXAMINE},
        {"CREATE", ImapEvent::CREATE},         {"DELETE", ImapEvent::DELETE},
        {"RENAME", ImapEvent::RENAME},         {"SUBSCRIBE", ImapEvent::SUBSCRIBE},
        {"UNSUBSCRIBE", ImapEvent::UNSUBSCRIBE}, {"LIST", ImapEvent::LIST},
        {"LSUB", ImapEvent::LSUB},             {"STATUS", ImapEvent::IMAP_STATUS},
        {"APPEND", ImapEvent::APPEND},         {"CHECK", ImapEvent::CHECK},
        {"CLOSE", ImapEvent::CLOSE},           {"EXPUNGE", ImapEvent::EXPUNGE},
        {"SEARCH", ImapEvent::SEARCH},         {"FETCH", ImapEvent::FETCH},
        {"STORE", ImapEvent::STORE},           {"COPY", ImapEvent::COPY},
        {"MOVE", ImapEvent::MOVE},             {"UID", ImapEvent::UID},
        {"NOOP", ImapEvent::NOOP},             {"IDLE", ImapEvent::IDLE},
        {"DONE", ImapEvent::DONE},             {"STARTTLS", ImapEvent::STARTTLS},
    };
    d.guards = {
        // INIT → NOT_AUTHENTICATED (on CONNECT)
        {ImapState::INIT, ImapEvent::CONNECT, ImapState::NOT_AUTHENTICATED},
        // NOT_AUTHENTICATED: stay on same state for most commands。LOGIN/
        // AUTHENTICATE 的自环行是"验证失败停留"出口；验证成功写 AUTHENTICATED
        // 是另一真实写点（旧表注释 "may transition in handler" 的漂移，
        // strict-guard 收集后补行，2026-10-26）。
        {ImapState::NOT_AUTHENTICATED, ImapEvent::CAPABILITY, ImapState::NOT_AUTHENTICATED},
        {ImapState::NOT_AUTHENTICATED, ImapEvent::LOGIN, ImapState::NOT_AUTHENTICATED},
        {ImapState::NOT_AUTHENTICATED, ImapEvent::LOGIN, ImapState::AUTHENTICATED},
        {ImapState::NOT_AUTHENTICATED, ImapEvent::AUTHENTICATE, ImapState::NOT_AUTHENTICATED},
        {ImapState::NOT_AUTHENTICATED, ImapEvent::AUTHENTICATE, ImapState::AUTHENTICATED},
        {ImapState::NOT_AUTHENTICATED, ImapEvent::NOOP, ImapState::NOT_AUTHENTICATED},
        {ImapState::NOT_AUTHENTICATED, ImapEvent::LOGOUT, ImapState::LOGOUT},
        // AUTHENTICATED
        {ImapState::AUTHENTICATED, ImapEvent::SELECT, ImapState::SELECTED},
        {ImapState::AUTHENTICATED, ImapEvent::EXAMINE, ImapState::SELECTED},
        {ImapState::AUTHENTICATED, ImapEvent::CAPABILITY, ImapState::AUTHENTICATED},
        {ImapState::AUTHENTICATED, ImapEvent::LIST, ImapState::AUTHENTICATED},
        {ImapState::AUTHENTICATED, ImapEvent::LSUB, ImapState::AUTHENTICATED},
        {ImapState::AUTHENTICATED, ImapEvent::IMAP_STATUS, ImapState::AUTHENTICATED},
        {ImapState::AUTHENTICATED, ImapEvent::CREATE, ImapState::AUTHENTICATED},
        {ImapState::AUTHENTICATED, ImapEvent::DELETE, ImapState::AUTHENTICATED},
        {ImapState::AUTHENTICATED, ImapEvent::RENAME, ImapState::AUTHENTICATED},
        {ImapState::AUTHENTICATED, ImapEvent::SUBSCRIBE, ImapState::AUTHENTICATED},
        {ImapState::AUTHENTICATED, ImapEvent::UNSUBSCRIBE, ImapState::AUTHENTICATED},
        {ImapState::AUTHENTICATED, ImapEvent::APPEND, ImapState::AUTHENTICATED},
        {ImapState::AUTHENTICATED, ImapEvent::CHECK, ImapState::AUTHENTICATED},
        {ImapState::AUTHENTICATED, ImapEvent::CLOSE, ImapState::AUTHENTICATED},
        {ImapState::AUTHENTICATED, ImapEvent::NOOP, ImapState::AUTHENTICATED},
        {ImapState::AUTHENTICATED, ImapEvent::IDLE, ImapState::AUTHENTICATED},
        // DONE 走 FSM 收口（同 IDLE：无 transition 则 dispatch 直接 BAD）
        {ImapState::AUTHENTICATED, ImapEvent::DONE, ImapState::AUTHENTICATED},
        {ImapState::AUTHENTICATED, ImapEvent::LOGOUT, ImapState::LOGOUT},
        // SELECTED
        {ImapState::SELECTED, ImapEvent::FETCH, ImapState::SELECTED},
        {ImapState::SELECTED, ImapEvent::STORE, ImapState::SELECTED},
        {ImapState::SELECTED, ImapEvent::SEARCH, ImapState::SELECTED},
        {ImapState::SELECTED, ImapEvent::COPY, ImapState::SELECTED},
        {ImapState::SELECTED, ImapEvent::MOVE, ImapState::SELECTED},
        {ImapState::SELECTED, ImapEvent::UID, ImapState::SELECTED},
        {ImapState::SELECTED, ImapEvent::EXPUNGE, ImapState::SELECTED},
        // RFC 3501 §6.3.1/§6.3.2：SELECT/EXAMINE 在 selected 态同样合法（切换邮箱）。
        // 缺失导致 APPEND 等命令之后同会话 SELECT 直接落 BAD（2026-10-21 生产挂死链一环）。
        {ImapState::SELECTED, ImapEvent::SELECT, ImapState::SELECTED},
        {ImapState::SELECTED, ImapEvent::EXAMINE, ImapState::SELECTED},
        {ImapState::SELECTED, ImapEvent::CLOSE, ImapState::AUTHENTICATED},
        {ImapState::SELECTED, ImapEvent::CHECK, ImapState::SELECTED},
        {ImapState::SELECTED, ImapEvent::CAPABILITY, ImapState::SELECTED},
        // RFC 3501 §6：SELECTED 态可用 AUTHENTICATED 态全部命令，STATUS 也不例外
        //（缺注册导致选中邮箱后 STATUS 落 BAD，Thunderbird 轮询其他邮箱状态即挂）。
        {ImapState::SELECTED, ImapEvent::IMAP_STATUS, ImapState::SELECTED},
        {ImapState::SELECTED, ImapEvent::NOOP, ImapState::SELECTED},
        {ImapState::SELECTED, ImapEvent::APPEND, ImapState::SELECTED},
        {ImapState::SELECTED, ImapEvent::IDLE, ImapState::SELECTED},
        {ImapState::SELECTED, ImapEvent::DONE, ImapState::SELECTED},
        {ImapState::SELECTED, ImapEvent::LOGOUT, ImapState::LOGOUT},
    };
    if (allow_starttls)
        d.guards.push_back({ImapState::NOT_AUTHENTICATED, ImapEvent::STARTTLS,
                            ImapState::NOT_AUTHENTICATED});

    // ERROR / TIMEOUT everywhere (remain in current state). Terminal states
    // (LOGOUT/CLOSED) need no rows: dispatch closes on them.
    for (int i = 0; i <= static_cast<int>(ImapState::SELECTED); ++i) {
        auto s = static_cast<ImapState>(i);
        d.guards.push_back({s, ImapEvent::ERROR, std::nullopt});
        d.guards.push_back({s, ImapEvent::TIMEOUT, std::nullopt});
    }

    // 兜底链最末位：正常路径由 FSM 注册的 session 感知 formatter 产生
    // tagged BAD（tag 在 ctx，desc 拿不到）。"* " 前缀是旧实现 ctx 缺失时的形态。
    d.error_policy = {"* BAD Invalid command sequence", 0};
    d.timeout_policy = {"", true};
    d.greeting = "* OK IMAP4rev1 Server Ready\r\n";
    d.internal_error_reply = "* NO Internal server error";
    // RFC 3501 §3：LOGOUT 与 CLOSED 都是终态（三协议中唯一双终态）。
    d.terminals = {ImapState::LOGOUT, ImapState::CLOSED};
    d.state_names = {"INIT", "NOT_AUTHENTICATED", "AUTHENTICATED", "SELECTED",
                     "LOGOUT", "CLOSED"};
    d.event_names = {
        "CONNECT", "CAPABILITY", "LOGIN",      "AUTHENTICATE", "LOGOUT",
        "SELECT",  "EXAMINE",     "CREATE",    "DELETE",       "RENAME",
        "SUBSCRIBE", "UNSUBSCRIBE", "LIST",    "LSUB",         "STATUS",
        "APPEND",  "CHECK",       "CLOSE",     "EXPUNGE",      "SEARCH",
        "FETCH",   "STORE",       "COPY",      "MOVE",         "UID",
        "NOOP",    "IDLE",        "DONE",      "STARTTLS",     "ERROR",
        "TIMEOUT",
    };
    return d;
}

} // namespace mail_system

#endif // IMAPS_DESC_H
