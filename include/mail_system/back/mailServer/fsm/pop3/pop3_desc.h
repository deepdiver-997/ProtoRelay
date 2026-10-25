#ifndef POP3_DESC_H
#define POP3_DESC_H
// POP3 protocol description for the fsm engine — pure data lifted verbatim
// from the retired FsmBase tables (traditional_pop3_fsm.tpp init_transition_
// table / hook overrides at main 18064f5). Only std types + the protocol's
// own enums: buildable without sessions or engines (design §3.4 boundary).
#include "framework/fsm/fsm_desc.h"
#include "mail_system/back/mailServer/fsm/pop3/pop3_types.hpp"

namespace mail_system {

inline pr::fsm::Desc<Pop3State, Pop3Event> make_pop3_desc() {
    pr::fsm::Desc<Pop3State, Pop3Event> d;
    // First-token table (session parse maps commands itself; the table is
    // the declarative mirror for future engine-side parsing).
    d.commands = {
        {"CAPA", Pop3Event::CAPA}, {"USER", Pop3Event::USER}, {"PASS", Pop3Event::PASS},
        {"STAT", Pop3Event::STAT}, {"LIST", Pop3Event::LIST}, {"UIDL", Pop3Event::UIDL},
        {"RETR", Pop3Event::RETR}, {"DELE", Pop3Event::DELE}, {"NOOP", Pop3Event::NOOP},
        {"RSET", Pop3Event::RSET}, {"QUIT", Pop3Event::QUIT},
    };
    d.guards = {
        {Pop3State::INIT, Pop3Event::CONNECT, Pop3State::AUTHORIZATION},
        {Pop3State::AUTHORIZATION, Pop3Event::USER, Pop3State::AUTHORIZATION},
        {Pop3State::AUTHORIZATION, Pop3Event::PASS, Pop3State::TRANSACTION},
        {Pop3State::AUTHORIZATION, Pop3Event::CAPA, Pop3State::AUTHORIZATION},
        {Pop3State::AUTHORIZATION, Pop3Event::NOOP, Pop3State::AUTHORIZATION},
        {Pop3State::AUTHORIZATION, Pop3Event::QUIT, Pop3State::UPDATE},
        {Pop3State::TRANSACTION, Pop3Event::STAT, Pop3State::TRANSACTION},
        {Pop3State::TRANSACTION, Pop3Event::LIST, Pop3State::TRANSACTION},
        {Pop3State::TRANSACTION, Pop3Event::UIDL, Pop3State::TRANSACTION},
        {Pop3State::TRANSACTION, Pop3Event::RETR, Pop3State::TRANSACTION},
        {Pop3State::TRANSACTION, Pop3Event::DELE, Pop3State::TRANSACTION},
        {Pop3State::TRANSACTION, Pop3Event::NOOP, Pop3State::TRANSACTION},
        {Pop3State::TRANSACTION, Pop3Event::RSET, Pop3State::TRANSACTION},
        {Pop3State::TRANSACTION, Pop3Event::CAPA, Pop3State::TRANSACTION},
        {Pop3State::TRANSACTION, Pop3Event::QUIT, Pop3State::UPDATE},
        // ERROR = unknown command / empty line: reply -ERR, stay (no close).
        {Pop3State::INIT, Pop3Event::ERROR, std::nullopt},
        {Pop3State::AUTHORIZATION, Pop3Event::ERROR, std::nullopt},
        {Pop3State::TRANSACTION, Pop3Event::ERROR, std::nullopt},
        {Pop3State::UPDATE, Pop3Event::ERROR, std::nullopt},
    };
    d.error_policy = {"-ERR Command not allowed in current state", 0};
    d.timeout_policy = {"", true};
    // Handler-missing fallback (was on_handler_not_found).
    d.format_missing = [](Pop3State, Pop3Event) {
        return std::string("-ERR Internal: handler missing");
    };
    // POP3's banner carries a timestamp — composed in handle_init_connect,
    // not by the engine.
    d.greeting.clear();
    d.internal_error_reply = "-ERR Internal: handler failed";
    d.terminals = {Pop3State::CLOSED};
    d.state_names = {"INIT", "AUTHORIZATION", "TRANSACTION", "UPDATE", "CLOSED"};
    d.event_names = {"CONNECT", "CAPA",  "USER", "PASS",    "STAT",
                     "LIST",   "UIDL",   "RETR", "DELE",    "NOOP",
                     "RSET",   "QUIT",   "ERROR", "TIMEOUT"};
    return d;
}

} // namespace mail_system

#endif // POP3_DESC_H
