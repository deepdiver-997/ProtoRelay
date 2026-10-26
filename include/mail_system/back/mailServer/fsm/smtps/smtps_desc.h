#ifndef SMTPS_DESC_H
#define SMTPS_DESC_H
// SMTP protocol description for the fsm engine — pure data lifted verbatim
// from the retired FsmBase tables (traditional_smtps_fsm.tpp init_transition_
// table / hook overrides at main 18064f5). The former `if constexpr` on the
// connection type becomes the allow_starttls parameter (SslConnection passes
// false: no STARTTLS row/handler on an already-TLS session). Only std types +
// the protocol's own enums: buildable without sessions or engines (design
// §3.4 boundary).
#include "framework/fsm/fsm_desc.h"
#include "mail_system/back/mailServer/fsm/smtps/smtps_types.hpp"

namespace mail_system {

inline pr::fsm::Desc<SmtpsState, SmtpsEvent> make_smtps_desc(bool allow_starttls) {
    pr::fsm::Desc<SmtpsState, SmtpsEvent> d;
    // First-token table (session parse maps commands itself; the table is
    // the declarative mirror for future engine-side parsing).
    d.commands = {
        {"EHLO", SmtpsEvent::EHLO},     {"HELO", SmtpsEvent::EHLO},
        {"AUTH", SmtpsEvent::AUTH},     {"MAIL FROM", SmtpsEvent::MAIL_FROM},
        {"RCPT TO", SmtpsEvent::RCPT_TO}, {"DATA", SmtpsEvent::DATA},
        {"QUIT", SmtpsEvent::QUIT},     {"STARTTLS", SmtpsEvent::STARTTLS},
    };
    d.guards = {
        // 两段 banner：先落 GREETING（CONNECT 行），写完 220 再落 WAIT_EHLO。
        // 第二段没有对应事件，补一行 CONNECT 文档行（CONNECT 在 GREETING 态
        // 不会再派发，死行仅作 strict-guard 断言目标）。
        {SmtpsState::INIT, SmtpsEvent::CONNECT, SmtpsState::GREETING},
        {SmtpsState::GREETING, SmtpsEvent::CONNECT, SmtpsState::WAIT_EHLO},
        {SmtpsState::WAIT_EHLO, SmtpsEvent::EHLO, SmtpsState::WAIT_AUTH},
        {SmtpsState::GREETING, SmtpsEvent::EHLO, SmtpsState::WAIT_AUTH},
        {SmtpsState::WAIT_AUTH, SmtpsEvent::EHLO, SmtpsState::WAIT_AUTH},
        // AUTH 的多出口：表首行是"下一提示态"主路径；PLAIN 一步/两步直通
        // WAIT_MAIL_FROM 与失败回 WAIT_AUTH 是真实写点（strict-guard 断言
        // 目标，同键多行不改变 dispatch 合法性判定）。
        {SmtpsState::WAIT_AUTH, SmtpsEvent::AUTH, SmtpsState::WAIT_AUTH_USERNAME},
        {SmtpsState::WAIT_AUTH, SmtpsEvent::AUTH, SmtpsState::WAIT_MAIL_FROM},
        {SmtpsState::WAIT_AUTH, SmtpsEvent::AUTH, SmtpsState::WAIT_AUTH},
        {SmtpsState::WAIT_AUTH_USERNAME, SmtpsEvent::AUTH, SmtpsState::WAIT_AUTH_PASSWORD},
        {SmtpsState::WAIT_AUTH_USERNAME, SmtpsEvent::AUTH, SmtpsState::WAIT_AUTH},
        {SmtpsState::WAIT_AUTH_PASSWORD, SmtpsEvent::AUTH, SmtpsState::WAIT_MAIL_FROM},
        {SmtpsState::WAIT_AUTH_PASSWORD, SmtpsEvent::AUTH, SmtpsState::WAIT_AUTH},
        // MAIL FROM：成功出口进 RCPT 阶段；策略拒绝（530/550/501）回
        // WAIT_AUTH 是三入口共用的失败写点（同键多行，不改变合法性判定）。
        {SmtpsState::WAIT_AUTH, SmtpsEvent::MAIL_FROM, SmtpsState::WAIT_RCPT_TO},
        {SmtpsState::WAIT_MAIL_FROM, SmtpsEvent::MAIL_FROM, SmtpsState::WAIT_RCPT_TO},
        {SmtpsState::WAIT_RCPT_TO, SmtpsEvent::MAIL_FROM, SmtpsState::WAIT_RCPT_TO},
        {SmtpsState::WAIT_MAIL_FROM, SmtpsEvent::MAIL_FROM, SmtpsState::WAIT_AUTH},
        {SmtpsState::WAIT_RCPT_TO, SmtpsEvent::MAIL_FROM, SmtpsState::WAIT_AUTH},
        {SmtpsState::WAIT_RCPT_TO, SmtpsEvent::RCPT_TO, SmtpsState::WAIT_RCPT_TO},
        {SmtpsState::WAIT_RCPT_TO, SmtpsEvent::DATA, SmtpsState::IN_MESSAGE},
        {SmtpsState::WAIT_DATA, SmtpsEvent::DATA, SmtpsState::IN_MESSAGE},
        {SmtpsState::IN_MESSAGE, SmtpsEvent::DATA, SmtpsState::IN_MESSAGE},
        {SmtpsState::IN_MESSAGE, SmtpsEvent::DATA_END, SmtpsState::WAIT_QUIT},
        {SmtpsState::WAIT_QUIT, SmtpsEvent::MAIL_FROM, SmtpsState::WAIT_RCPT_TO},
    };
    if (allow_starttls)
        d.guards.push_back({SmtpsState::WAIT_AUTH, SmtpsEvent::STARTTLS, SmtpsState::INIT});

    for (int i = 0; i < static_cast<int>(SmtpsState::CLOSED); ++i) {
        // QUIT 全态 → CLOSED。旧表此行的 to 从未被执行（handler 靠写回调里
        // close 兜底、不 set 态）；迁移后 handle_wait_quit_quit 以
        // apply_closing(CLOSED) 真正落地这一行。
        d.guards.push_back({static_cast<SmtpsState>(i), SmtpsEvent::QUIT, SmtpsState::CLOSED});
        // ERROR / TIMEOUT / EMPTY 自环（EMPTY = 空行静默吞掉，2026-10-26 分离）。
        d.guards.push_back({static_cast<SmtpsState>(i), SmtpsEvent::ERROR, std::nullopt});
        d.guards.push_back({static_cast<SmtpsState>(i), SmtpsEvent::TIMEOUT, std::nullopt});
        d.guards.push_back({static_cast<SmtpsState>(i), SmtpsEvent::EMPTY, std::nullopt});
    }

    // on_invalid_transition: 500 + set_error(InvalidCommand)（set_error 经引擎
    // violation observer 补上）。不计 stay_times_（与 handle_error 的 ERROR
    // 预算分开），budget=0。
    d.error_policy = {"500 Error: Invalid command sequence", 0};
    d.timeout_policy = {"", true};
    // 守卫命中而 handler 缺失：旧 FsmBase::on_handler_not_found 默认静默且
    // 实际不可达（守卫/handler 表组合完整）；引擎兜底回 error_policy.reply。
    d.greeting.clear();  // banner 由 handle_init_connect 两段写出
    d.internal_error_reply = "500 4.0.0 Internal server error";
    d.terminals = {SmtpsState::CLOSED};
    d.state_names = {"INIT",         "GREETING",       "WAIT_EHLO",
                     "WAIT_AUTH",    "WAIT_AUTH_USERNAME", "WAIT_AUTH_PASSWORD",
                     "WAIT_MAIL_FROM", "WAIT_RCPT_TO",  "WAIT_DATA",
                     "IN_MESSAGE",   "WAIT_QUIT",      "CLOSED"};
    d.event_names = {"CONNECT", "EHLO",    "AUTH",     "MAIL_FROM", "RCPT_TO",
                     "DATA",     "DATA_END", "QUIT",    "STARTTLS",  "ERROR",
                     "TIMEOUT",  "EMPTY"};
    return d;
}

} // namespace mail_system

#endif // SMTPS_DESC_H
