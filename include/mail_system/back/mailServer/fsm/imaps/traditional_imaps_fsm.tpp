#ifndef TRADITIONAL_IMAPS_FSM_TPP
#define TRADITIONAL_IMAPS_FSM_TPP

#include "mail_system/back/mailServer/imaps_server.h"
#include "mail_system/back/mailServer/fsm/imaps/imap_utils.h"
#include "mail_system/back/common/mime_parser.h"
#include "mail_system/back/common/mail_crypto.h"
#include "mail_system/back/common/password_rehash.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <cstring>
#include <filesystem>
#include <regex>
#include <unordered_map>
#include <string>
#include <utility>
#include <vector>

namespace mail_system {

// 纯工具函数（响应构建/编解码/序列解析）已拆到 imap_utils.h/.cpp（非模板）。
// 这里 using 让 FSM 处理器里原有的非限定调用（build_flags_string 等）直接解析。
using namespace imap_utils;

// ====================================================================

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::send_untagged(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session,
    const std::string& data)
{
    session->do_async_write("* " + data + "\r\n",
        [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code& ec) {
            if (!ec) s->do_async_read();
        }
    );
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::send_tagged(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session,
    const std::string& tag,
    const std::string& status,
    const std::string& message)
{
    session->do_async_write(tag + " " + status + " " + message + "\r\n",
        [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code& ec) {
            if (!ec) s->do_async_read();
        }
    );
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::send_continuation(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session,
    const std::string& message)
{
    session->do_async_write("+ " + message + "\r\n",
        nullptr
    );
}

// ====================================================================
// 初始化：转换表 / 处理器
// ====================================================================

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::init_transition_table() {
    // INIT → NOT_AUTHENTICATED (on CONNECT)
    this->add_transition(ImapState::INIT, ImapEvent::CONNECT, ImapState::NOT_AUTHENTICATED);

    // NOT_AUTHENTICATED: stay on same state for most commands
    this->add_transition(ImapState::NOT_AUTHENTICATED, ImapEvent::CAPABILITY, ImapState::NOT_AUTHENTICATED);
    this->add_transition(ImapState::NOT_AUTHENTICATED, ImapEvent::LOGIN, ImapState::NOT_AUTHENTICATED); // may transition in handler
    this->add_transition(ImapState::NOT_AUTHENTICATED, ImapEvent::AUTHENTICATE, ImapState::NOT_AUTHENTICATED);
    this->add_transition(ImapState::NOT_AUTHENTICATED, ImapEvent::NOOP, ImapState::NOT_AUTHENTICATED);
    this->add_transition(ImapState::NOT_AUTHENTICATED, ImapEvent::LOGOUT, ImapState::LOGOUT);
    if constexpr (!std::is_same_v<ConnectionType, pr::SslConnection>)
        this->add_transition(ImapState::NOT_AUTHENTICATED, ImapEvent::STARTTLS, ImapState::NOT_AUTHENTICATED);

    // AUTHENTICATED
    this->add_transition(ImapState::AUTHENTICATED, ImapEvent::SELECT, ImapState::SELECTED);
    this->add_transition(ImapState::AUTHENTICATED, ImapEvent::EXAMINE, ImapState::SELECTED);
    this->add_transition(ImapState::AUTHENTICATED, ImapEvent::CAPABILITY, ImapState::AUTHENTICATED);
    this->add_transition(ImapState::AUTHENTICATED, ImapEvent::LIST, ImapState::AUTHENTICATED);
    this->add_transition(ImapState::AUTHENTICATED, ImapEvent::LSUB, ImapState::AUTHENTICATED);
    this->add_transition(ImapState::AUTHENTICATED, ImapEvent::IMAP_STATUS, ImapState::AUTHENTICATED);
    this->add_transition(ImapState::AUTHENTICATED, ImapEvent::CREATE, ImapState::AUTHENTICATED);
    this->add_transition(ImapState::AUTHENTICATED, ImapEvent::DELETE, ImapState::AUTHENTICATED);
    this->add_transition(ImapState::AUTHENTICATED, ImapEvent::RENAME, ImapState::AUTHENTICATED);
    this->add_transition(ImapState::AUTHENTICATED, ImapEvent::SUBSCRIBE, ImapState::AUTHENTICATED);
    this->add_transition(ImapState::AUTHENTICATED, ImapEvent::UNSUBSCRIBE, ImapState::AUTHENTICATED);
    this->add_transition(ImapState::AUTHENTICATED, ImapEvent::APPEND, ImapState::AUTHENTICATED);
    this->add_transition(ImapState::AUTHENTICATED, ImapEvent::CHECK, ImapState::AUTHENTICATED);
    this->add_transition(ImapState::AUTHENTICATED, ImapEvent::CLOSE, ImapState::AUTHENTICATED);
    this->add_transition(ImapState::AUTHENTICATED, ImapEvent::NOOP, ImapState::AUTHENTICATED);
    this->add_transition(ImapState::AUTHENTICATED, ImapEvent::IDLE, ImapState::AUTHENTICATED);
    this->add_transition(ImapState::AUTHENTICATED, ImapEvent::LOGOUT, ImapState::LOGOUT);

    // SELECTED
    this->add_transition(ImapState::SELECTED, ImapEvent::FETCH, ImapState::SELECTED);
    this->add_transition(ImapState::SELECTED, ImapEvent::STORE, ImapState::SELECTED);
    this->add_transition(ImapState::SELECTED, ImapEvent::SEARCH, ImapState::SELECTED);
    this->add_transition(ImapState::SELECTED, ImapEvent::COPY, ImapState::SELECTED);
    this->add_transition(ImapState::SELECTED, ImapEvent::MOVE, ImapState::SELECTED);
    this->add_transition(ImapState::SELECTED, ImapEvent::UID, ImapState::SELECTED);
    this->add_transition(ImapState::SELECTED, ImapEvent::EXPUNGE, ImapState::SELECTED);
    // RFC 3501 §6.3.1/§6.3.2：SELECT/EXAMINE 在 selected 态同样合法（切换邮箱）。
    // 缺失导致 APPEND 等命令之后同会话 SELECT 直接落 BAD（2026-10-21 生产实测挂死链一环）。
    this->add_transition(ImapState::SELECTED, ImapEvent::SELECT, ImapState::SELECTED);
    this->add_transition(ImapState::SELECTED, ImapEvent::EXAMINE, ImapState::SELECTED);
    this->add_transition(ImapState::SELECTED, ImapEvent::CLOSE, ImapState::AUTHENTICATED);
    this->add_transition(ImapState::SELECTED, ImapEvent::CHECK, ImapState::SELECTED);
    this->add_transition(ImapState::SELECTED, ImapEvent::CAPABILITY, ImapState::SELECTED);
    this->add_transition(ImapState::SELECTED, ImapEvent::NOOP, ImapState::SELECTED);
    this->add_transition(ImapState::SELECTED, ImapEvent::APPEND, ImapState::SELECTED);
    this->add_transition(ImapState::SELECTED, ImapEvent::IDLE, ImapState::SELECTED);
    this->add_transition(ImapState::SELECTED, ImapEvent::LOGOUT, ImapState::LOGOUT);

    // ERROR / TIMEOUT everywhere (remain in current state)
    for (int i = 0; i <= static_cast<int>(ImapState::SELECTED); ++i) {
        auto s = static_cast<ImapState>(i);
        this->add_transition(s, ImapEvent::ERROR, s);
        this->add_transition(s, ImapEvent::TIMEOUT, s);
    }
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::init_state_handlers() {
    // INIT
    this->add_handler(ImapState::INIT, ImapEvent::CONNECT, [this](auto session) { handle_init_connect(session); });

    // NOT_AUTHENTICATED
    this->add_handler(ImapState::NOT_AUTHENTICATED, ImapEvent::CAPABILITY, [this](auto session) { handle_capability(session); });
    if constexpr (!std::is_same_v<ConnectionType, pr::SslConnection>)
        this->add_handler(ImapState::NOT_AUTHENTICATED, ImapEvent::STARTTLS, [this](auto session) { handle_starttls(session); });
    this->add_handler(ImapState::NOT_AUTHENTICATED, ImapEvent::LOGIN, [this](auto session) { handle_login(session); });
    this->add_handler(ImapState::NOT_AUTHENTICATED, ImapEvent::AUTHENTICATE, [this](auto session) { handle_authenticate(session); });
    this->add_handler(ImapState::NOT_AUTHENTICATED, ImapEvent::NOOP, [this](auto session) { handle_noop(session); });
    this->add_handler(ImapState::NOT_AUTHENTICATED, ImapEvent::LOGOUT, [this](auto session) { handle_logout(session); });
    this->add_handler(ImapState::NOT_AUTHENTICATED, ImapEvent::ERROR, [this](auto session) { handle_error(session); });
    this->add_handler(ImapState::NOT_AUTHENTICATED, ImapEvent::TIMEOUT, [this](auto session) { handle_timeout(session); });

    // AUTHENTICATED
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::CAPABILITY, [this](auto session) { handle_capability(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::SELECT, [this](auto session) { handle_select(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::EXAMINE, [this](auto session) { handle_examine(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::LIST, [this](auto session) { handle_list(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::LSUB, [this](auto session) { handle_lsub(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::IMAP_STATUS, [this](auto session) { handle_status(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::CREATE, [this](auto session) { handle_create(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::DELETE, [this](auto session) { handle_delete(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::RENAME, [this](auto session) { handle_rename(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::SUBSCRIBE, [this](auto session) { handle_subscribe(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::UNSUBSCRIBE, [this](auto session) { handle_unsubscribe(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::APPEND, [this](auto session) { handle_append(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::CHECK, [this](auto session) { handle_check(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::CLOSE, [this](auto session) { handle_close(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::NOOP, [this](auto session) { handle_noop(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::IDLE, [this](auto session) { handle_idle(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::LOGOUT, [this](auto session) { handle_logout(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::ERROR, [this](auto session) { handle_error(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::TIMEOUT, [this](auto session) { handle_timeout(session); });

    // SELECTED
    this->add_handler(ImapState::SELECTED, ImapEvent::FETCH, [this](auto session) { handle_fetch(session, false); });
    this->add_handler(ImapState::SELECTED, ImapEvent::STORE, [this](auto session) { handle_store(session); });
    this->add_handler(ImapState::SELECTED, ImapEvent::SEARCH, [this](auto session) { handle_search(session, false); });
    this->add_handler(ImapState::SELECTED, ImapEvent::COPY, [this](auto session) { handle_copy(session); });
    this->add_handler(ImapState::SELECTED, ImapEvent::MOVE, [this](auto session) { handle_move(session); });
    this->add_handler(ImapState::SELECTED, ImapEvent::UID, [this](auto session) { handle_uid(session); });
    this->add_handler(ImapState::SELECTED, ImapEvent::EXPUNGE, [this](auto session) { handle_expunge(session); });
    this->add_handler(ImapState::SELECTED, ImapEvent::SELECT, [this](auto session) { handle_select(session); });
    this->add_handler(ImapState::SELECTED, ImapEvent::EXAMINE, [this](auto session) { handle_examine(session); });
    this->add_handler(ImapState::SELECTED, ImapEvent::CLOSE, [this](auto session) { handle_close(session); });
    this->add_handler(ImapState::SELECTED, ImapEvent::CHECK, [this](auto session) { handle_check(session); });
    this->add_handler(ImapState::SELECTED, ImapEvent::APPEND, [this](auto session) { handle_append(session); });
    this->add_handler(ImapState::SELECTED, ImapEvent::CAPABILITY, [this](auto session) { handle_capability(session); });
    this->add_handler(ImapState::SELECTED, ImapEvent::NOOP, [this](auto session) { handle_noop(session); });
    this->add_handler(ImapState::SELECTED, ImapEvent::IDLE, [this](auto session) { handle_idle(session); });
    this->add_handler(ImapState::SELECTED, ImapEvent::LOGOUT, [this](auto session) { handle_logout(session); });
    this->add_handler(ImapState::SELECTED, ImapEvent::ERROR, [this](auto session) { handle_error(session); });
    this->add_handler(ImapState::SELECTED, ImapEvent::TIMEOUT, [this](auto session) { handle_timeout(session); });
}

// ====================================================================
// 事件派发
// ====================================================================

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::process_event(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session,
    ImapEvent event,
    const std::string& tag)
{
    if constexpr (ENABLE_IMAP_DETAIL_DEBUG_LOG) {
        LOG_IMAP_DETAIL_DEBUG("Current State: {}, Event: {}, Tag: {}",
                          TraditionalImapsFsm<ConnectionType>::get_state_name(
                              static_cast<ImapState>(session->get_current_state())),
                          TraditionalImapsFsm<ConnectionType>::get_event_name(event),
                          tag);
    }

    // 保存 tag 到 context
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    if (ctx) {
        ctx->current_tag = tag;
    }

    this->dispatch(session, static_cast<ImapState>(session->get_current_state()), event);
}
template <typename ConnectionType>
bool TraditionalImapsFsm<ConnectionType>::is_terminal_state(ImapState s) const {
    return s == ImapState::LOGOUT || s == ImapState::CLOSED;
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::on_invalid_transition(
    ImapState, ImapEvent,
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    send_tagged(session, ctx ? ctx->current_tag : "*", "BAD", "Invalid command sequence");
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::on_handler_not_found(
    ImapState, ImapEvent,
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    send_tagged(session, ctx ? ctx->current_tag : "*", "BAD", "Unsupported command in current state");
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::invoke_handler(
    typename pr::FsmBase<ConnectionType, ImapState, ImapEvent>::Handler& h,
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    try {
        h(session);
    } catch (const std::exception& e) {
        auto* ctx = static_cast<ImapContext*>(session->get_context());
        LOG_IMAP_ERROR("IMAP handler exception: {} tag={}", e.what(),
                       ctx ? ctx->current_tag : "?");
        send_tagged(session, ctx ? ctx->current_tag : "*", "NO", "Internal server error");
    } catch (...) {
        auto* ctx = static_cast<ImapContext*>(session->get_context());
        LOG_IMAP_ERROR("IMAP handler unknown exception tag={}",
                       ctx ? ctx->current_tag : "?");
        send_tagged(session, ctx ? ctx->current_tag : "*", "NO", "Internal server error");
    }
}


template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::auto_process_event(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    if (!ctx) return;

    // 如果是 IDLE 状态的特殊处理
    if (ctx->idle_mode) {
        handle_done(session);
        return;
    }

    // 从 session 获取待处理事件
    // 注意：session 中 event 和 args 在 parse 阶段已设置好
    // 这里需要一个适配: 从 session 获取 event type 和 args
    // ImapsSession 会在 parse 时设置这些值
    // 但现在 process_event 签名多了 tag 参数，auto_process_event 需要适配

    // 对于 IMAP，会话解析完命令后直接调用 process_event，
    // 这里留空作为兼容接口
}

// ====================================================================
// 状态处理器实现
// ====================================================================

// ---------- INIT → CONNECT → greeting ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_init_connect(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));

    // 发送 IMAP 欢迎语
    session->do_async_write("* OK IMAP4rev1 Server Ready\r\n",
        [](std::shared_ptr<pr::SessionBase<ConnectionType>> self,
           const std::error_code& ec) mutable {
            if (ec) {
                LOG_IMAP_ERROR("Error sending IMAP greeting: {}", ec.message());
                return;
            }
            LOG_IMAP_DEBUG("Sent IMAP greeting, waiting for commands...");
            self->do_async_read();
        }
    );
}

// ---------- CAPABILITY ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_capability(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    std::string caps = "* CAPABILITY IMAP4rev1";
    // 非 SSL 连接可以通告 STARTTLS
    if constexpr (!std::is_same_v<ConnectionType, pr::SslConnection>) {
        caps += " STARTTLS";
    }
    caps += " IDLE UIDPLUS MOVE AUTH=PLAIN\r\n";

    caps += tag + " OK CAPABILITY completed\r\n";

    session->do_async_write(caps,
        nullptr
    );
}

// ---------- LOGIN ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_login(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    // 解析 "username password"
    std::string username, password, args = session->get_last_command_args();
    size_t space = args.find(' ');
    if (space != std::string::npos && space > 0) {
        // 可能带引号
        if (args[0] == '"') {
            size_t end_quote = args.find('"', 1);
            if (end_quote != std::string::npos) {
                username = args.substr(1, end_quote - 1);
                password = args.substr(end_quote + 2); // skip " and space
            }
        } else {
            username = args.substr(0, space);
            password = args.substr(space + 1);
        }
        // trim password
        if (!password.empty() && password[0] == '"') {
            size_t end = password.find('"', 1);
            if (end != std::string::npos) {
                password = password.substr(1, end - 1);
            }
        }
    } else {
        username = args;
    }

    start_auth_verify(session, tag, std::move(username), std::move(password),
                      "LOGIN completed", "LOGIN failed");
}

// ---------- LOGIN/AUTHENTICATE 共用验证链 ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::start_auth_verify(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session,
    const std::string& tag, std::string username, std::string password,
    const char* ok_text, const char* fail_text)
{
    // 自动补 @domain（兼容只传本地部分的客户端）
    if (!username.empty() && username.find('@') == std::string::npos) {
        auto config = std::atomic_load(&session->get_server()->m_config);
        username += "@" + config->system_domain;
        LOG_IMAP_DEBUG("Auto-domain applied: {}@{}", username, config->system_domain);
    }

    // bcrypt 是几十到几百毫秒的纯 CPU：绝不能在共享 io 线程上算 ——
    // 那会把同线程上所有其他会话的读写/握手/定时器全部卡住。
    // pause 后把「查 DB + 验密码」整体丢给 worker 线程池；回调在 worker
    // 线程触发，pause 期间独占 session（SPF/DNS/commit 回调同一约定）。
    // 登录者自己的延迟不变（bcrypt 该算多久还是多久），变的是别人不再陪等。
    auto router = this->m_shardRouter;
    auto auth_cache = this->m_authCache;
    auto worker = this->m_workerThreadPool;
    if (!worker) {
        send_tagged(session, tag, "NO", "Server not ready");
        return;
    }
    session->set_paused(true);
    auto self = session->shared_from_this();
    worker->post(
        [self, tag, username = std::move(username), password = std::move(password),
         router, auth_cache, ok_text, fail_text]() mutable {
            // async CPS 链（仿 POP3/SMTP auth）：每步 async_query + 回调续作。
            // 底层 MySQL async_* 是默认同步包装（回调内联执行），链在 worker
            // 线程跑完；将来接真异步 DB，回调在 DB 线程触发，conn 由链内
            // shared ScopedConnection 保活，调用方结构无需改动。
            TraditionalImapsFsm<ConnectionType>::auth_user_async(
                router, auth_cache, username, password,
                [self, tag, username = username, ok_text, fail_text](
                    bool ok, uint64_t user_id, int shard) mutable {
                    if (!self || self->is_closed()) return;
                    if (ok) {
                        auto* c = static_cast<ImapContext*>(self->get_context());
                        if (c) {
                            c->is_authenticated = true;
                            c->username = username;
                            c->user_id = user_id;
                            c->shard_index = shard;
                        }
                        self->set_authenticated(true); // close()/trace/metrics 不能等析构才同步
                        self->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
                        send_tagged(self, tag, "OK", ok_text);
                        LOG_IMAP_INFO("IMAP login successful: {} (user_id={})", username, user_id);
                    } else {
                        LOG_IMAP_WARN("IMAP login failed: {}", username);
                        if (self->record_auth_failure_and_check()) {
                            self->close_after_flush();   // 阈值关闭走 io 线程,且不与在途写竞争
                            return;
                        }
                        send_tagged(self, tag, "NO", fail_text);
                    }
                    self->drain_buffered_commands();
                });
        });
}

// ---------- AUTHENTICATE ----------
// 支持 SASL PLAIN（含 SASL-IR 初始响应与 "*" 中止）；其余机制维持 NO。
// 无初始响应时发空挑战 "+ "，续行由 session handle_read 拦截后回到本 handler。
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_authenticate(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    // 续行到达（handle_read 已拦截存入 context）
    if (ctx && ctx->sasl_plain_state == 2) {
        ctx->sasl_plain_state = 0;
        std::string resp = std::move(ctx->sasl_plain_response);
        ctx->sasl_plain_response.clear();
        if (resp == "*") {   // RFC 3501 §6.2.2: 客户端可随时以 "*" 中止
            send_tagged(session, tag, "NO", "AUTHENTICATE aborted");
            return;
        }
        verify_sasl_plain(session, tag, resp, "AUTHENTICATE completed");
        return;
    }

    const std::string args = session->get_last_command_args();
    std::string mechanism = args;
    std::string initial_response;
    size_t space = args.find(' ');
    if (space != std::string::npos) {
        mechanism = args.substr(0, space);
        initial_response = args.substr(space + 1);
    }
    std::transform(mechanism.begin(), mechanism.end(), mechanism.begin(), ::toupper);

    if (mechanism != "PLAIN") {
        send_tagged(session, tag, "NO", "Unsupported authentication mechanism: " + mechanism);
        return;
    }
    if (!ctx) {
        send_tagged(session, tag, "NO", "Server error");
        return;
    }

    // SASL-IR：初始响应随命令行一起到达
    if (!initial_response.empty()) {
        verify_sasl_plain(session, tag, initial_response, "AUTHENTICATE completed");
        return;
    }

    // 无初始响应：发空挑战等续行（RFC 3501 §6.2.2 的 "+ "）
    ctx->sasl_plain_state = 1;
    ctx->sasl_plain_response.clear();
    session->do_async_write("+ \r\n", nullptr);
}

// 解码 SASL PLAIN 消息（authzid NUL authcid NUL passwd）并复用登录验证链。
// base64 解码失败（EVP 返回负）得空串、缺分隔符或多余分段 → NO；
// authzid 忽略（不做代理授权），以 authcid 为认证身份。
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::verify_sasl_plain(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session,
    const std::string& tag, const std::string& b64, const char* ok_text)
{
    const std::string decoded = outbound::base64_decode(b64);
    size_t n1 = decoded.find('\0');
    size_t n2 = (n1 == std::string::npos) ? std::string::npos
                                          : decoded.find('\0', n1 + 1);
    if (n1 == std::string::npos || n2 == std::string::npos ||
        decoded.find('\0', n2 + 1) != std::string::npos) {
        send_tagged(session, tag, "NO", "AUTHENTICATE failed");
        return;
    }
    std::string username = decoded.substr(n1 + 1, n2 - n1 - 1);
    std::string password = decoded.substr(n2 + 1);
    start_auth_verify(session, tag, std::move(username), std::move(password),
                      ok_text, "AUTHENTICATE failed");
}

// ---------- LOGOUT ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_logout(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string current_tag = ctx ? ctx->current_tag : "*";
    ctx->clear();

    session->set_trace_clean_close();   // 正常 LOGOUT → 连接追踪丢弃
    session->set_current_state(static_cast<int>(ImapState::LOGOUT));

    std::string response = "* BYE IMAP4rev1 Server logging out\r\n";
    response += current_tag + " OK LOGOUT completed\r\n";

    session->do_async_write(response,
        [](std::shared_ptr<pr::SessionBase<ConnectionType>> s,
           const std::error_code& ec) mutable {
            if (ec) {
                LOG_IMAP_ERROR("Error sending LOGOUT reply: {}", ec.message());
                return;
            }
            auto timer = s->get_connection().executor()->make_timer();
            timer->expires_after(std::chrono::milliseconds(100));
            timer->async_wait([s = std::move(s), timer](const std::error_code& error) mutable {
                if (!error) s->close();
            });
        }
    );
}

// ---------- SELECT ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_select(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    std::string mailbox_name = session->get_last_command_args();
    if (!mailbox_name.empty() && mailbox_name[0] == '"') {
        size_t end = mailbox_name.find('"', 1);
        if (end != std::string::npos)
            mailbox_name = mailbox_name.substr(1, end - 1);
    }
    mailbox_name = imap_utils::decode_mailbox_name(mailbox_name);

    if (!ctx || !ctx->is_authenticated) {
        send_tagged(session, tag, "BAD", "Not authenticated");
        return;
    }

    auto conn = this->acquire_connection(ctx->shard_index);
    if (!conn->is_valid()) {
        send_tagged(session, tag, "NO", "Server database unavailable");
        return;
    }
    session->set_paused(true);
    auto self = session->shared_from_this();
    uint64_t user_id = ctx->user_id;

    // CPS 链：找邮箱 id → 统计（缓存感知）→ 组装 SELECT 响应
    TraditionalImapsFsm<ConnectionType>::find_mailbox_id_async(
        conn, user_id, mailbox_name,
        [this, self, tag = std::move(tag), conn, user_id,
         mailbox_name = mailbox_name](uint64_t mailbox_id) mutable {
            if (!self || self->is_closed()) return;
            auto* ctx = static_cast<ImapContext*>(self->get_context());
            if (mailbox_id == 0) {
                send_tagged(self, tag, "NO", "Mailbox not found: " + mailbox_name);
                self->drain_buffered_commands();
                return;
            }

            // Save selected mailbox info
            ctx->mailbox_selected = true;
            ctx->selected_mailbox_name = mailbox_name;
            ctx->selected_mailbox_id = mailbox_id;
            ctx->read_only = false;
            // Generate UIDVALIDITY (use mailbox_id as validity)
            ctx->uid_validity = mailbox_id;

            this->get_mailbox_stats_cached_async(
                conn, user_id, mailbox_id,
                [self, tag = std::move(tag), ctx](MailboxCacheEntry stats, bool, bool) {
                    if (!self || self->is_closed()) return;
                    size_t count = stats.exists;
                    size_t unseen = stats.unseen;
                    uint64_t uidnext = stats.uidnext;

                    self->set_current_state(static_cast<int>(ImapState::SELECTED));

                    // Build SELECT response (RFC 3501: [READ-WRITE]/[READ-ONLY] on tagged OK)
                    std::string response;
                    response += "* " + std::to_string(count) + " EXISTS\r\n";
                    // 我们没有跟踪 \Recent 标志（自上次访问后到达的邮件），报告 0。
                    // 之前用 count - unseen（已读数量）是错的：只要有已读邮件就恒 > 0，
                    // 导致客户端一直认为有新邮件而反复重同步。
                    response += "* 0 RECENT\r\n";
                    if (unseen > 0) {
                        response += "* OK [UNSEEN " + std::to_string(count - unseen + 1) + "]\r\n";
                    }
                    response += "* OK [UIDVALIDITY " + std::to_string(ctx->uid_validity) + "]\r\n";
                    response += "* OK [UIDNEXT " + std::to_string(uidnext) + "]\r\n";
                    response += tag + " OK [";
                    response += ctx->read_only ? "READ-ONLY" : "READ-WRITE";
                    response += "] SELECT completed\r\n";

                    self->do_async_write(response, [](auto s, auto& ec) { if (!ec) s->do_async_read(); });
                    self->drain_buffered_commands();
                });
        });
}

// ---------- EXAMINE ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_examine(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    std::string mailbox_name = session->get_last_command_args();
    if (!mailbox_name.empty() && mailbox_name[0] == '"') {
        size_t end = mailbox_name.find('"', 1);
        if (end != std::string::npos) {
            mailbox_name = mailbox_name.substr(1, end - 1);
        }
    }

    if (!ctx || !ctx->is_authenticated) {
        send_tagged(session, tag, "BAD", "Not authenticated");
        return;
    }

    auto conn = this->acquire_connection(ctx->shard_index);
    if (!conn->is_valid()) {
        send_tagged(session, tag, "NO", "Server database unavailable");
        return;
    }
    session->set_paused(true);
    auto self = session->shared_from_this();
    uint64_t user_id = ctx->user_id;

    // CPS 链：找邮箱 id → 统计 → 组装 EXAMINE 响应
    TraditionalImapsFsm<ConnectionType>::find_mailbox_id_async(
        conn, user_id, mailbox_name,
        [this, self, tag = std::move(tag), conn, user_id,
         mailbox_name = mailbox_name](uint64_t mailbox_id) mutable {
            if (!self || self->is_closed()) return;
            auto* ctx = static_cast<ImapContext*>(self->get_context());
            if (mailbox_id == 0) {
                send_tagged(self, tag, "NO", "Mailbox not found: " + mailbox_name);
                self->drain_buffered_commands();
                return;
            }

            ctx->mailbox_selected = true;
            ctx->selected_mailbox_name = mailbox_name;
            ctx->selected_mailbox_id = mailbox_id;
            ctx->read_only = true;
            ctx->uid_validity = mailbox_id;

            this->get_mailbox_stats_cached_async(
                conn, user_id, mailbox_id,
                [self, tag = std::move(tag), ctx](MailboxCacheEntry stats, bool, bool) {
                    if (!self || self->is_closed()) return;
                    size_t count = stats.exists;
                    size_t unseen = stats.unseen;
                    uint64_t uidnext = stats.uidnext;

                    self->set_current_state(static_cast<int>(ImapState::SELECTED));

                    std::string response;
                    response += "* " + std::to_string(count) + " EXISTS\r\n";
                    response += "* 0 RECENT\r\n";
                    if (unseen > 0) {
                        response += "* OK [UNSEEN " + std::to_string(count - unseen + 1) + "]\r\n";
                    }
                    response += "* OK [UIDVALIDITY " + std::to_string(ctx->uid_validity) + "]\r\n";
                    response += "* OK [UIDNEXT " + std::to_string(uidnext) + "]\r\n";
                    response += "* OK [READ-ONLY]\r\n";
                    response += tag + " OK EXAMINE completed\r\n";

                    self->do_async_write(response, [](auto s, auto& ec) { if (!ec) s->do_async_read(); });
                    self->drain_buffered_commands();
                });
        });
}

// ---------- LIST ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_list(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated) {
        send_tagged(session, tag, "BAD", "Not authenticated");
        return;
    }

    // LIST 参数: reference mailbox_name
    // 简单实现：列出用户所有邮箱
    auto conn = this->acquire_connection(ctx->shard_index);
    if (!conn->is_valid()) {
        send_tagged(session, tag, "NO", "Server database unavailable");
        return;
    }
    session->set_paused(true);
    auto self = session->shared_from_this();
    uint64_t user_id = ctx->user_id;

    TraditionalImapsFsm<ConnectionType>::get_mailboxes_async(
        conn, user_id,
        [self, tag = std::move(tag)](bool ok,
            std::vector<std::tuple<uint64_t, std::string, int>> mailboxes) mutable {
            if (!self || self->is_closed()) return;
            if (!ok) {
                send_tagged(self, tag, "OK", "LIST completed");
                self->drain_buffered_commands();
                return;
            }
            std::string response;
            for (const auto& mb : mailboxes) {
                const std::string& name = std::get<1>(mb);
                int box_type = std::get<2>(mb);

                std::string encoded_name = encode_mailbox_name(name);

                // 如果是收件箱（box_type=1），也要以 INBOX 形式呈现
                if (box_type == 1) {
                    // 发送 INBOX 和中文名两个条目
                    response += "* LIST (\\HasNoChildren) \"/\" INBOX\r\n";
                    response += "* LIST (\\HasNoChildren) \"/\" " + quote_string(encoded_name) + "\r\n";
                    continue;
                }
                std::string attrs = "()";
                response += "* LIST " + attrs + " \"/\" " + quote_string(encoded_name) + "\r\n";
            }
            response += tag + " OK LIST completed\r\n";

            self->do_async_write(response, [](auto s, auto& ec) { if (!ec) s->do_async_read(); });
            self->drain_buffered_commands();
        });
}

// ---------- LSUB ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_lsub(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated) {
        send_tagged(session, tag, "BAD", "Not authenticated");
        return;
    }

    // Simplified: return all mailboxes as subscribed
    auto conn = this->acquire_connection(ctx->shard_index);
    if (!conn->is_valid()) {
        send_tagged(session, tag, "NO", "Server database unavailable");
        return;
    }
    session->set_paused(true);
    auto self = session->shared_from_this();
    uint64_t user_id = ctx->user_id;

    TraditionalImapsFsm<ConnectionType>::get_mailboxes_async(
        conn, user_id,
        [self, tag = std::move(tag)](bool,
            std::vector<std::tuple<uint64_t, std::string, int>> mailboxes) mutable {
            if (!self || self->is_closed()) return;
            std::string response;
            for (const auto& mb : mailboxes) {
                const std::string& name = std::get<1>(mb);
                int box_type = std::get<2>(mb);
                std::string encoded_name = encode_mailbox_name(name);

                if (box_type == 1) {
                    response += "* LSUB (\\HasNoChildren) \"/\" INBOX\r\n";
                }
                response += "* LSUB () \"/\" " + quote_string(encoded_name) + "\r\n";
            }
            response += tag + " OK LSUB completed\r\n";

            self->do_async_write(response, [](auto s, auto& ec) { if (!ec) s->do_async_read(); });
            self->drain_buffered_commands();
        });
}

// ---------- STATUS ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_status(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated) {
        send_tagged(session, tag, "BAD", "Not authenticated");
        return;
    }

    // Parse "mailbox (MESSAGES UNSEEN UIDNEXT UIDVALIDITY)"
    std::string args = session->get_last_command_args();
    std::string mailbox_name = args;
    std::string status_attrs;

    size_t paren_open = args.find('(');
    if (paren_open != std::string::npos) {
        mailbox_name = args.substr(0, paren_open);
        // trim
        mailbox_name.erase(mailbox_name.find_last_not_of(" \t") + 1);
        size_t paren_close = args.find(')', paren_open);
        if (paren_close != std::string::npos) {
            status_attrs = args.substr(paren_open + 1, paren_close - paren_open - 1);
        }
    }

    // Trim quotes from mailbox name
    if (!mailbox_name.empty() && mailbox_name[0] == '"') {
        size_t end = mailbox_name.find('"', 1);
        if (end != std::string::npos) {
            mailbox_name = mailbox_name.substr(1, end - 1);
        }
    }

    auto conn = this->acquire_connection(ctx->shard_index);
    if (!conn->is_valid()) {
        send_tagged(session, tag, "NO", "Server database unavailable");
        return;
    }
    session->set_paused(true);
    auto self = session->shared_from_this();
    uint64_t user_id = ctx->user_id;

    // CPS 链：找邮箱 id → 统计 → 组装 STATUS 响应
    TraditionalImapsFsm<ConnectionType>::find_mailbox_id_async(
        conn, user_id, mailbox_name,
        [this, self, tag = std::move(tag), conn, user_id,
         mailbox_name = mailbox_name,
         status_attrs = std::move(status_attrs)](uint64_t mailbox_id) mutable {
            if (!self || self->is_closed()) return;
            if (mailbox_id == 0) {
                send_tagged(self, tag, "NO", "Mailbox not found");
                self->drain_buffered_commands();
                return;
            }
            this->get_mailbox_stats_cached_async(
                conn, user_id, mailbox_id,
                [self, tag = std::move(tag), mailbox_id, mailbox_name = std::move(mailbox_name),
                 status_attrs = std::move(status_attrs)](MailboxCacheEntry stats, bool, bool) mutable {
                    if (!self || self->is_closed()) return;
                    size_t messages = stats.exists;
                    size_t unseen = stats.unseen;
                    uint64_t uidnext = stats.uidnext;
                    uint64_t uidvalidity = mailbox_id;

                    std::transform(status_attrs.begin(), status_attrs.end(), status_attrs.begin(), ::toupper);

                    std::string response = "* STATUS " + quote_string(encode_mailbox_name(mailbox_name)) + " (";
                    if (status_attrs.find("MESSAGES") != std::string::npos || status_attrs.empty()) {
                        response += "MESSAGES " + std::to_string(messages) + " ";
                    }
                    if (status_attrs.find("UNSEEN") != std::string::npos || status_attrs.empty()) {
                        response += "UNSEEN " + std::to_string(unseen) + " ";
                    }
                    if (status_attrs.find("UIDNEXT") != std::string::npos || status_attrs.empty()) {
                        response += "UIDNEXT " + std::to_string(uidnext) + " ";
                    }
                    if (status_attrs.find("UIDVALIDITY") != std::string::npos || status_attrs.empty()) {
                        response += "UIDVALIDITY " + std::to_string(uidvalidity) + " ";
                    }
                    // Remove trailing space and close
                    if (response.back() == ' ') response.pop_back();
                    response += ")\r\n";
                    response += tag + " OK STATUS completed\r\n";

                    self->do_async_write(response, [](auto s, auto& ec) { if (!ec) s->do_async_read(); });
                    self->drain_buffered_commands();
                });
        });
}

// ---------- FETCH ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_fetch(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session, bool is_uid)
{
    LOG_IMAP_INFO("FETCH ENTER args=[{}]", session->get_last_command_args());
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated || !ctx->mailbox_selected) {
        send_tagged(session, tag, "BAD", "No mailbox selected");
        return;
    }

    // Parse: <sequence-set> <message-data-item-names>
    auto args = (ctx && ctx->is_uid_command) ? ctx->uid_overridden_args : session->get_last_command_args();
    size_t space = args.find(' ');
    if (space == std::string::npos) {
        send_tagged(session, tag, "BAD", "FETCH requires arguments");
        return;
    }

    std::string seq_set = args.substr(0, space);
    std::string attrs = args.substr(space + 1);
    if (!attrs.empty() && attrs[0] == '(') {
        size_t close = attrs.find(')');
        if (close != std::string::npos) {
            attrs = attrs.substr(1, close - 1);
        }
    }

    uint64_t mailbox_id = ctx->selected_mailbox_id;
    uint64_t user_id = ctx->user_id;

    // 读路径快表：列表缓存命中（TTL 内且未被写命令失效）→ 整个 FETCH 免查库。
    // 无 DB 段所以无需 pause；fetch_finalize 自带 drain + 续读，与非 pause 兼容。
    // 失效语义见 MailboxListCache 注释（写命令显式失效 + TTL 兜底）。
    if (m_mailboxListCache) {
        MailboxListEntry lentry;
        bool lstale = false;
        if (m_mailboxListCache->get(mbox_cache_key(user_id, mailbox_id), lentry, lstale) && !lstale) {
            fetch_from_mails(session, std::move(tag), is_uid,
                             std::move(seq_set), std::move(attrs), std::move(lentry.mails));
            return;
        }
    }

    // 读路径 DB 查询走 CPS 链：查完在回调里继续组装 FETCH，并回填列表缓存。
    auto conn = this->acquire_connection(ctx->shard_index);
    if (!conn->is_valid()) {
        send_tagged(session, tag, "NO", "Server database unavailable");
        return;
    }
    session->set_paused(true);
    auto self = session->shared_from_this();

    TraditionalImapsFsm<ConnectionType>::get_mailbox_mails_async(
        conn, mailbox_id, user_id,
        [this, self, conn, tag = std::move(tag), is_uid,
         seq_set = std::move(seq_set), attrs = std::move(attrs),
         mailbox_id, user_id](
            bool ok, std::vector<MailboxMailInfo> mails) mutable {
            if (!self || self->is_closed()) return;
            if (!ok || mails.empty()) {
                send_tagged(self, tag, "OK", "FETCH completed (empty)");
                self->drain_buffered_commands();
                return;
            }
            // 回填列表缓存：TTL + 写命令显式失效保证一致性；大列表不缓存
            //（单条 MB 级会撑爆 LRU），超大邮箱维持每轮查库。
            if (m_mailboxListCache && mails.size() <= kMailboxListCacheMaxMails) {
                m_mailboxListCache->put(mbox_cache_key(user_id, mailbox_id),
                                        MailboxListEntry{mails});
            }
            fetch_from_mails(self, std::move(tag), is_uid,
                             std::move(seq_set), std::move(attrs), std::move(mails));
        });
}

// DB 慢路径回调与列表缓存快路径共用的 FETCH 组装/驱动（原 handle_fetch 回调体）。
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::fetch_from_mails(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session,
    std::string tag, bool is_uid,
    std::string seq_set, std::string attrs,
    std::vector<MailboxMailInfo> mails)
{
    auto self = session;
    std::transform(attrs.begin(), attrs.end(), attrs.begin(), ::toupper);

    bool want_uid = is_uid || attrs.find("UID") != std::string::npos;
    bool want_flags = attrs.find("FLAGS") != std::string::npos || attrs.find("ALL") != std::string::npos || attrs.find("FAST") != std::string::npos;
    bool want_internaldate = attrs.find("INTERNALDATE") != std::string::npos || attrs.find("ALL") != std::string::npos;
    bool want_rfc822_size = attrs.find("RFC822.SIZE") != std::string::npos || attrs.find("ALL") != std::string::npos || attrs.find("FAST") != std::string::npos;
    bool want_envelope = attrs.find("ENVELOPE") != std::string::npos || attrs.find("ALL") != std::string::npos;
    bool has_header_fields = attrs.find("HEADER.FIELDS") != std::string::npos;
    bool want_body_header = has_header_fields ||
        attrs.find("BODY.PEEK[HEADER]") != std::string::npos ||
        attrs.find("BODY[HEADER]") != std::string::npos;
    std::string header_fields_filter;
    bool header_fields_not = false;
    if (has_header_fields) {
        header_fields_not = attrs.find("HEADER.FIELDS.NOT") != std::string::npos;
        size_t lp = attrs.find('(', attrs.find("HEADER.FIELDS"));
        if (lp != std::string::npos) {
            size_t rp = attrs.find(')', lp);
            if (rp != std::string::npos)
                header_fields_filter = attrs.substr(lp + 1, rp - lp - 1);
        }
    }
    // 一条 FETCH 可同时索取多个 section，按请求顺序输出。两类：
    //   BODY[]                          → 整封（path 空，kind=full，inside=""）
    //   BODY[n]/BODY[n.m…]/后缀（.MIME/.HEADER/.HEADER.FIELDS(.NOT)/.TEXT）
    //                                   → parse_body_section（message/rfc822 取址）
    // 顶层 BODY[HEADER…]/HEADER.FIELDS 不以数字开头，不进此循环，
    // 仍走下方 want_body_header 既有路径（行为不变）。
    std::vector<imap_utils::ImapBodySection> body_sections;
    {
        // 捕获组可有可无：BODY[] 组不参与（整封），BODY[2…] 组捕获括号内原文
        static const std::regex section_pattern(R"(BODY(?:\.PEEK)?\[(\d[^\]]*)?\])");
        for (std::sregex_iterator it(attrs.begin(), attrs.end(), section_pattern), end; it != end; ++it) {
            imap_utils::ImapBodySection sec;
            const bool numbered = (*it)[1].matched;
            if (numbered && !imap_utils::parse_body_section((*it)[1].str(), sec)) {
                send_tagged(self, tag, "BAD", "Invalid BODY section");
                self->drain_buffered_commands();
                return;
            }
            body_sections.push_back(std::move(sec));
        }
    }
    const bool want_body = !body_sections.empty();
    bool want_body_struct = attrs.find("BODYSTRUCTURE") != std::string::npos;

    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    expand_seq_set(seq_set, mails.size(), ranges);
    if (ranges.empty()) {
        send_tagged(self, tag, "OK", "FETCH completed");
        self->drain_buffered_commands();
        return;
    }

    auto state = std::make_shared<FetchContext>();
    state->tag = tag;
    state->mails = std::move(mails);
    state->ranges = std::move(ranges);
    state->want_uid = want_uid;
    state->want_flags = want_flags;
    state->want_internaldate = want_internaldate;
    state->want_rfc822_size = want_rfc822_size;
    state->want_envelope = want_envelope;
    state->want_body = want_body;
    state->want_body_header = want_body_header;
    state->want_body_struct = want_body_struct;
    state->has_header_fields = has_header_fields;
    state->header_fields_not = header_fields_not;
    state->header_fields_filter = std::move(header_fields_filter);
    state->body_sections = std::move(body_sections);
    auto* srv = self->get_server();
    state->provider = srv->m_shardRouter ? srv->m_shardRouter->get_storage(0) : nullptr;

    // 正文读取走 async：本地内联（行为与同步版一致），远程后端由装饰器
    // 投递 worker —— 逐封 size/正文不再阻塞 io 线程；每封正文只读一次，
    // header/body/BODYSTRUCTURE 兜底共用（旧路径最多读三次）。
    fetch_drive(self, state);
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::invalidate_mailbox_list(
    uint64_t user_id, uint64_t mailbox_id)
{
    // flags 变化与删信都不推进 uidnext，列表缓存只能靠写命令显式失效保一致；
    // TTL 只兜跨节点/跨协议改库（SMTP 投递、POP3 DELE）的底。
    if (m_mailboxListCache) {
        m_mailboxListCache->invalidate(mbox_cache_key(user_id, mailbox_id));
    }
}

// ---------- FETCH 续作链 ----------

template <typename ConnectionType>
struct TraditionalImapsFsm<ConnectionType>::FetchContext {
    std::string tag;
    std::vector<MailboxMailInfo> mails;
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    size_t range_idx = 0;
    uint64_t cur_seq = 0;             // 0 = 待推进到下一封
    std::string cur;                  // 当前邮件的响应片段
    std::string response;
    std::shared_ptr<storage::IStorageProvider> provider;
    std::string cur_body_path;        // 供异步回调里的日志
    bool struct_sidecar_hit = false;
    MimePart struct_tree;

    bool want_uid = false, want_flags = false, want_internaldate = false;
    bool want_rfc822_size = false, want_envelope = false;
    bool want_body = false, want_body_header = false, want_body_struct = false;
    bool has_header_fields = false, header_fields_not = false;
    std::string header_fields_filter;
    std::vector<imap_utils::ImapBodySection> body_sections;

    // 推进游标到下一封待处理的邮件；false = 全部完成
    bool advance() {
        for (;;) {
            if (cur_seq == 0) {
                if (range_idx >= ranges.size()) return false;
                cur_seq = ranges[range_idx].first;
            }
            if (cur_seq > ranges[range_idx].second || cur_seq > mails.size()) {
                ++range_idx;
                cur_seq = 0;
                continue;
            }
            return true;
        }
    }
};

// 无 provider 时的本地兜底（与原 read_mail_body 的 ifstream 分支一致）
static std::string fetch_read_local_body(const std::string& body_path) {
    if (body_path.empty()) return "";
    std::ifstream in(body_path, std::ios::binary);
    if (!in.is_open()) {
        LOG_FILE_IO_ERROR("Failed to open mail body: {}", body_path);
        return "";
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// 用已就绪的正文完成当前邮件剩余 item（纯数据推进，不触碰 session）
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::fetch_complete_mail_with_body(
    FetchContext& c, std::string body_content)
{
    const auto& mail_info = c.mails[c.cur_seq - 1];

    if (c.want_body_header) {
        std::string headers = body_content;
        size_t hdr_end = body_content.find("\r\n\r\n");
        if (hdr_end != std::string::npos)
            headers = body_content.substr(0, hdr_end + 2);

        std::string label = "BODY[HEADER]";
        if (c.has_header_fields && !c.header_fields_filter.empty()) {
            std::set<std::string> wanted;
            {
                std::istringstream fs(c.header_fields_filter);
                std::string f;
                while (fs >> f) {
                    std::transform(f.begin(), f.end(), f.begin(), ::tolower);
                    wanted.insert(f);
                }
            }
            std::string filtered;
            std::istringstream hs(headers);
            std::string line;
            while (std::getline(hs, line)) {
                if (line.empty() || line == "\r") break;
                if (line.back() == '\r') line.pop_back();
                size_t colon = line.find(':');
                if (colon != std::string::npos) {
                    std::string hdr_name = line.substr(0, colon);
                    std::transform(hdr_name.begin(), hdr_name.end(), hdr_name.begin(), ::tolower);
                    bool match = wanted.count(hdr_name) > 0;
                    if (c.header_fields_not) match = !match;
                    if (match) filtered += line + "\r\n";
                }
            }
            if (!filtered.empty() && filtered.size() >= 2)
                filtered.resize(filtered.size() - 2);
            headers = filtered;
            label = c.header_fields_not ? "BODY[HEADER.FIELDS.NOT (" : "BODY[HEADER.FIELDS (";
            label += c.header_fields_filter + ")]";
        }
        c.cur += label + " " + build_fetch_body_response(headers, headers.size()) + " ";
    }
    if (c.want_body) {
        MimePart mime_tree;
        const bool need_parts = std::any_of(c.body_sections.begin(), c.body_sections.end(),
                                            [](const imap_utils::ImapBodySection& s) {
                                                return !s.path.empty();
                                            });
        const bool have_tree = need_parts && ensure_mime_tree(mail_info.body_path, body_content, mime_tree);
        for (const auto& section : c.body_sections) {
            std::string part_content;
            const std::string* content = &body_content;
            if (!section.path.empty()) {
                // BODY[]（path 空）→ 整封；编号 section → RFC 3501 §6.4.5 取址
                // （multipart 子部件 / rfc822 内嵌消息、.MIME/.HEADER/.TEXT 等）
                if (have_tree) {
                    part_content = imap_utils::extract_body_section(body_content, mime_tree, section);
                    content = &part_content;
                } else {
                    content = &part_content;   // 树不可用 → 空 literal（与旧行为一致）
                }
            }
            const std::string label = "BODY[" + section.inside + "]";
            c.cur += label + " " + build_fetch_body_response(*content, content->size()) + " ";
        }
    }
    if (c.want_body_struct) {
        if (c.struct_sidecar_hit) {
            c.cur += "BODYSTRUCTURE " + build_bodystructure_tree(c.struct_tree, body_content) + " ";
        } else if (ensure_mime_tree(mail_info.body_path, body_content, c.struct_tree)) {
            c.cur += "BODYSTRUCTURE " + build_bodystructure_tree(c.struct_tree, body_content) + " ";
        } else {
            c.cur += "BODYSTRUCTURE " + build_bodystructure(body_content) + " ";
        }
    }

    if (!c.cur.empty() && c.cur.back() == ' ') c.cur.pop_back();
    c.cur += ")\r\n";
    c.response += c.cur;
    c.cur.clear();
    ++c.cur_seq;
}

// size 之后的阶段：envelope（内存）→ 决定是否读正文 → 完成本封。
// 返回 true 表示已发起异步正文读取（回调里 fetch_continue）；false 表示正文已
// 同步就绪并完成本封。
template <typename ConnectionType>
bool TraditionalImapsFsm<ConnectionType>::fetch_after_size(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session,
    std::shared_ptr<FetchContext> ctx,
    std::shared_ptr<std::atomic<bool>> alive)
{
    const auto& mail_info = ctx->mails[ctx->cur_seq - 1];

    if (ctx->want_envelope) {
        std::string date_str;
        {
            struct tm result;
            memset(&result, 0, sizeof(result));
            time_t t = mail_info.send_time;
#ifdef _WIN32
            gmtime_s(&result, &t);
#else
            gmtime_r(&t, &result);
#endif
            static const char* months[] = {
                "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
            };
            char buf[32];
            snprintf(buf, sizeof(buf), "%02d-%s-%04d",
                     result.tm_mday, months[result.tm_mon],
                     result.tm_year + 1900);
            date_str = buf;
        }
        ctx->cur += "ENVELOPE " + build_envelope_string(
            date_str, mail_info.subject, mail_info.sender, mail_info.sender,
            "", mail_info.recipient, "", "", "",
            std::to_string(mail_info.mail_id)) + " ";
    }

    // BODYSTRUCTURE 先试 sidecar（本地小文件，同步读即可）
    ctx->struct_sidecar_hit = false;
    if (ctx->want_body_struct) {
        ctx->struct_sidecar_hit = load_mime_tree(mail_info.body_path, ctx->struct_tree) &&
                                  ctx->struct_tree.metadata_version >= kMimeMetadataVersion;
    }
    // rfc822 部件的 BODYSTRUCTURE 需要内嵌消息头（ENVELOPE），sidecar 只存结构
    // 不存头文本 → 树里含 rfc822 时必须读正文（否则 ENVELOPE 只能全 NIL）。
    const bool need_body = !mail_info.body_path.empty() &&
        (ctx->want_body_header || ctx->want_body ||
         (ctx->want_body_struct && !ctx->struct_sidecar_hit) ||
         (ctx->want_body_struct && ctx->struct_sidecar_hit &&
          imap_utils::mime_tree_has_rfc822(ctx->struct_tree)));

    if (need_body && ctx->provider) {
        session->set_paused(true);
        ctx->cur_body_path = mail_info.body_path;
        auto provider = ctx->provider;
        provider->async_read_all(mail_info.body_path,
            [session, ctx, alive](bool ok, std::string data, const storage::IoError& error) mutable {
                if (!ok) {
                    LOG_FILE_IO_ERROR("Failed to read mail body {}: {}",
                                      ctx->cur_body_path, error.message);
                    data.clear();
                }
                fetch_complete_mail_with_body(*ctx, std::move(data));
                fetch_continue(session, ctx, alive);   // inline→外层循环续；deferred→驱动下一封
            });
        return true;
    }
    std::string body = need_body ? fetch_read_local_body(mail_info.body_path) : std::string();
    fetch_complete_mail_with_body(*ctx, std::move(body));
    return false;
}

// 完成本封剩余 item 后的续作：inline 回调（外层 fetch_drive 帧仍在）→ 外层循环继续；
// deferred 回调（外层已返回）→ 由这里驱动下一封。alive 每封一个，size 与 body 两个
// 异步读共享，保证"最后完成本封的回调"恰好驱动/续作一次。
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::fetch_continue(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session,
    std::shared_ptr<FetchContext> ctx,
    std::shared_ptr<std::atomic<bool>> alive)
{
    if (alive->exchange(false)) return;   // inline：外层 fetch_drive 会 continue
    fetch_drive(session, ctx);            // deferred：驱动下一封
}

// 驱动器：迭代化续作链（不递归）。同步路径在 for 循环里逐封完成；需要异步
// size/正文读取时发起后：若回调 inline（外层帧仍在）→ 循环 continue；若回调
// deferred（外层已返回）→ 由回调里的 fetch_continue 驱动下一封。
// alive 每封一个共享标记，size 与 body 两个异步读共用，保证续作恰好一次。
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::fetch_drive(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session,
    std::shared_ptr<FetchContext> ctx)
{
    for (;;) {
        if (!ctx->advance()) {
            fetch_finalize(session, ctx);
            return;
        }
        const auto& mail_info = ctx->mails[ctx->cur_seq - 1];

        ctx->cur = "* " + std::to_string(ctx->cur_seq) + " FETCH (";
        if (ctx->want_uid) {
            ctx->cur += "UID " + std::to_string(mail_info.mail_id) + " ";
        }
        if (ctx->want_flags) {
            std::string flags = build_flags_string(
                mail_info.status, mail_info.is_starred,
                mail_info.is_deleted, mail_info.is_important);
            ctx->cur += "FLAGS (" + flags + ") ";
        }
        if (ctx->want_internaldate) {
            ctx->cur += "INTERNALDATE \"" + imap_timestamp(mail_info.send_time) + "\" ";
        }

        // 本封的续作标记：size / body 两个异步读共享
        auto alive = std::make_shared<std::atomic<bool>>(true);

        if (ctx->want_rfc822_size) {
            if (!mail_info.body_path.empty() && ctx->provider) {
                session->set_paused(true);
                ctx->cur_body_path = mail_info.body_path;
                auto provider = ctx->provider;
                provider->async_object_size(mail_info.body_path,
                    [session, ctx, alive](bool ok, std::uint64_t sz, const storage::IoError& error) mutable {
                        if (!ok) {
                            LOG_FILE_IO_ERROR("RFC822.SIZE lookup failed for {}: {}",
                                              ctx->cur_body_path, error.message);
                            sz = 0;
                        }
                        ctx->cur += "RFC822.SIZE " + std::to_string(sz) + " ";
                        // 正文可能再异步读；只有最后完成本封的一步才续作
                        if (fetch_after_size(session, ctx, alive)) return;
                        fetch_continue(session, ctx, alive);
                    });
                if (alive->exchange(false)) return;   // deferred：回调会 fetch_continue 驱动
                continue;                             // inline：本封已完成，下一封
            }
            std::uint64_t sz = 0;
            if (!mail_info.body_path.empty()) {
                std::error_code ec;
                const auto fsz = std::filesystem::file_size(mail_info.body_path, ec);
                sz = ec ? 0 : static_cast<std::uint64_t>(fsz);
            }
            ctx->cur += "RFC822.SIZE " + std::to_string(sz) + " ";
        }
        if (fetch_after_size(session, ctx, alive)) {
            if (alive->exchange(false)) return;   // deferred：正文回调驱动
            continue;                             // inline：本封已完成
        }
        // 同步完成本封 → 循环处理下一封
    }
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::fetch_finalize(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session,
    std::shared_ptr<FetchContext> ctx)
{
    std::string response = std::move(ctx->response);
    response += ctx->tag + " OK FETCH completed\r\n";

    session->do_async_write(response,
        [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code& ec) {
            if (ec) return;
            // 异步链期间流水线被 pause：先排空缓冲，再视情况续读
            s->drain_buffered_commands();
            if (!s->has_buffered_input() && !s->is_paused() && !s->is_closed()) s->do_async_read();
        });
}

// ---------- STORE ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_store(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated || !ctx->mailbox_selected) {
        send_tagged(session, tag, "BAD", "No mailbox selected");
        return;
    }

    // Parse: <sequence-set> <data-item> <value>
    // e.g. "1:* +FLAGS (\\Seen \\Flagged)"
    //      "2:4 FLAGS (\\Deleted)"
    auto* ctx_store = static_cast<ImapContext*>(session->get_context());
    auto args = (ctx_store && ctx_store->is_uid_command) ? ctx_store->uid_overridden_args : session->get_last_command_args();
    size_t space1 = args.find(' ');
    if (space1 == std::string::npos) {
        send_tagged(session, tag, "BAD", "STORE requires arguments");
        return;
    }

    std::string seq_set = args.substr(0, space1);
    std::string rest = args.substr(space1 + 1);

    // Handle optional SILENT
    bool silent = false;
    size_t space2 = rest.find(' ');
    if (space2 == std::string::npos) {
        send_tagged(session, tag, "BAD", "STORE requires flags");
        return;
    }

    std::string store_cmd = rest.substr(0, space2);
    std::string flags_part = rest.substr(space2 + 1);

    // Check for SILENT（大小写不敏感）
    std::string cmd_upper = store_cmd;
    std::transform(cmd_upper.begin(), cmd_upper.end(), cmd_upper.begin(), ::toupper);
    if (cmd_upper.find("SILENT") != std::string::npos) {
        silent = true;
        // 注意顺序：先匹配带 +/- 的，再匹配裸 FLAGS，否则 -FLAGS 会丢失负号被当成添加
        if (cmd_upper.find("-FLAGS") != std::string::npos) {
            store_cmd = "-FLAGS";
        } else if (cmd_upper.find("+FLAGS") != std::string::npos) {
            store_cmd = "+FLAGS";
        } else if (cmd_upper.find("FLAGS") != std::string::npos) {
            store_cmd = "FLAGS";
        }
    }

    // Parse flags from parentheses
    if (!flags_part.empty() && flags_part[0] == '(') {
        size_t close = flags_part.find(')');
        if (close != std::string::npos) {
            flags_part = flags_part.substr(1, close - 1);
        }
    }

    // RFC 3501 属性名大小写不敏感：客户端可能发 \SEEN / \Seen / \seen
    // （之前只匹配 \Seen，网易大师发 \SEEN 时 flag_seen=false → 已读从不落库）
    std::string flags_upper = flags_part;
    std::transform(flags_upper.begin(), flags_upper.end(), flags_upper.begin(), ::toupper);
    bool flag_seen = flags_upper.find("\\SEEN") != std::string::npos;
    bool flag_flagged = flags_upper.find("\\FLAGGED") != std::string::npos;
    bool flag_deleted = flags_upper.find("\\DELETED") != std::string::npos;
    bool add = store_cmd.find('+') != std::string::npos || (store_cmd.find("FLAGS") != std::string::npos && store_cmd[0] != '-');
    bool remove = store_cmd.find('-') != std::string::npos;

    // 批量 flag 更新（STORE）：读路径 CPS 链查邮件 + 收件人，然后
    // 单条 IN 列表 SQL 批量写（拍平原 N 次循环 update_mail_seen/...）。
    auto conn = this->acquire_connection(ctx->shard_index);
    if (!conn->is_valid()) {
        send_tagged(session, tag, "NO", "Server database unavailable");
        return;
    }
    session->set_paused(true);
    auto self = session->shared_from_this();
    uint64_t mailbox_id = ctx->selected_mailbox_id;
    uint64_t user_id = ctx->user_id;

    TraditionalImapsFsm<ConnectionType>::get_mailbox_mails_async(
        conn, mailbox_id, user_id,
        [this, self, conn, tag = std::move(tag), seq_set = std::move(seq_set),
         flag_seen, flag_flagged, flag_deleted, add, remove, silent,
         mailbox_id, user_id](bool ok, std::vector<MailboxMailInfo> mails) mutable {
            if (!self || self->is_closed()) return;
            if (!ok || mails.empty()) {
                send_tagged(self, tag, "OK", "STORE completed");
                self->drain_buffered_commands();
                return;
            }

            // Parse sequence ranges（支持逗号分隔）
            std::vector<std::pair<uint64_t, uint64_t>> ranges;
            expand_seq_set(seq_set, mails.size(), ranges);
            if (ranges.empty()) {
                send_tagged(self, tag, "OK", "STORE completed");
                self->drain_buffered_commands();
                return;
            }

            // 收件人（seen 写 mail_recipients.status 需要）；查完再继续
            TraditionalImapsFsm<ConnectionType>::get_user_email_async(
                conn, user_id,
                [this, self, conn, tag = std::move(tag), mails = std::move(mails),
                 ranges = std::move(ranges), flag_seen, flag_flagged, flag_deleted,
                 add, remove, silent, mailbox_id, user_id](
                    std::string user_email) mutable {
                    if (!self || self->is_closed()) return;
                    if (user_email.empty()) {
                        auto* c = static_cast<ImapContext*>(self->get_context());
                        if (c) user_email = c->username;
                    }

                    // 收集每个 flag 操作的 mail_id 集合（add/remove 互斥，各至多一次批量）
                    std::vector<uint64_t> seen_ids, flagged_ids, deleted_ids;
                    std::string response;
                    for (const auto& range : ranges) {
                    for (uint64_t seq = range.first; seq <= range.second; ++seq) {
                        size_t idx = seq - 1;
                        const auto& mail_info = mails[idx];

                        if ((add || !remove) && flag_seen) seen_ids.push_back(mail_info.mail_id);
                        if (remove && flag_seen) seen_ids.push_back(mail_info.mail_id);
                        if ((add || !remove) && flag_flagged) flagged_ids.push_back(mail_info.mail_id);
                        if (remove && flag_flagged) flagged_ids.push_back(mail_info.mail_id);
                        if ((add || !remove) && flag_deleted) deleted_ids.push_back(mail_info.mail_id);
                        if (remove && flag_deleted) deleted_ids.push_back(mail_info.mail_id);

                        if (!silent) {
                            int new_status = (add && flag_seen) ? 0 : mail_info.status;
                            if (remove && flag_seen) new_status = 1;

                            std::string flags = build_flags_string(
                                new_status,
                                (add && flag_flagged) ? true : (remove && flag_flagged) ? false : mail_info.is_starred,
                                (add && flag_deleted) ? true : (remove && flag_deleted) ? false : mail_info.is_deleted,
                                mail_info.is_important);
                            response += "* " + std::to_string(seq) + " FETCH (FLAGS (" + flags + "))\r\n";
                        }
                    }
                    } // for each range

                    // 批量写：seen / flagged / deleted 各一条 IN SQL，串行执行。
                    // 完成后失效统计缓存并回包。
                    int seen_status = (add && flag_seen) ? 0 : 1;
                    std::vector<std::function<void(std::function<void(bool)>)>> steps;
                    if (!seen_ids.empty()) {
                        auto ids = std::move(seen_ids);
                        auto recipient = user_email;
                        int status = seen_status;
                        steps.push_back([conn, ids = std::move(ids), recipient = std::move(recipient), status](
                                            std::function<void(bool)> done) {
                            TraditionalImapsFsm<ConnectionType>::batch_mark_seen_async(
                                conn, ids, recipient, status, std::move(done));
                        });
                    }
                    if (!flagged_ids.empty()) {
                        auto ids = std::move(flagged_ids);
                        int v = add ? 1 : 0;
                        steps.push_back([conn, ids = std::move(ids), mailbox_id, user_id, v](
                                            std::function<void(bool)> done) {
                            TraditionalImapsFsm<ConnectionType>::batch_mark_flagged_async(
                                conn, ids, user_id, mailbox_id, v, std::move(done));
                        });
                    }
                    if (!deleted_ids.empty()) {
                        auto ids = std::move(deleted_ids);
                        int v = add ? 1 : 0;
                        steps.push_back([conn, ids = std::move(ids), mailbox_id, user_id, v](
                                            std::function<void(bool)> done) {
                            TraditionalImapsFsm<ConnectionType>::batch_mark_deleted_async(
                                conn, ids, user_id, mailbox_id, v, std::move(done));
                        });
                    }

                    auto run_next = std::make_shared<std::function<void(size_t)>>();
                    *run_next = [this, self, tag = std::move(tag), response = std::move(response),
                                 steps = std::move(steps), mailbox_id, user_id,
                                 run_next](size_t i) mutable {
                        if (i >= steps.size()) {
                            // 全部批量完成：失效统计缓存与列表缓存，回包
                            if (this->m_mailboxStatsCache) {
                                this->m_mailboxStatsCache->invalidate(mbox_cache_key(user_id, mailbox_id));
                            }
                            this->invalidate_mailbox_list(user_id, mailbox_id);
                            response += tag + " OK STORE completed\r\n";
                            self->do_async_write(response, [](auto s, auto& ec) { if (!ec) s->do_async_read(); });
                            self->drain_buffered_commands();
                            return;
                        }
                        steps[i]([run_next, i](bool) { (*run_next)(i + 1); });
                    };
                    (*run_next)(0);
                });
        });
}

// ---------- EXPUNGE ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_expunge(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated || !ctx->mailbox_selected) {
        send_tagged(session, tag, "BAD", "No mailbox selected");
        return;
    }

    // 读邮件列表 → 算 \Deleted 序号 → 批量物理删除 → 回 EXPUNGE 通知
    auto conn = this->acquire_connection(ctx->shard_index);
    if (!conn->is_valid()) {
        send_tagged(session, tag, "NO", "Server database unavailable");
        return;
    }
    session->set_paused(true);
    auto self = session->shared_from_this();
    uint64_t mailbox_id = ctx->selected_mailbox_id;
    uint64_t user_id = ctx->user_id;

    TraditionalImapsFsm<ConnectionType>::get_mailbox_mails_async(
        conn, mailbox_id, user_id,
        [this, self, conn, tag = std::move(tag), mailbox_id, user_id](
            bool ok, std::vector<MailboxMailInfo> mails) mutable {
            if (!self || self->is_closed()) return;
            if (!ok) {
                send_tagged(self, tag, "NO", "Server error");
                self->drain_buffered_commands();
                return;
            }

            // Find which sequences are deleted
            std::vector<uint64_t> expunged_seqs;
            for (size_t i = 0; i < mails.size(); ++i) {
                if (mails[i].is_deleted) {
                    expunged_seqs.push_back(i + 1);
                }
            }

            // Actually delete from database
            TraditionalImapsFsm<ConnectionType>::expunge_mailbox_async(
                conn, mailbox_id, user_id,
                [this, self, tag = std::move(tag), expunged_seqs = std::move(expunged_seqs),
                 mailbox_id, user_id](bool) {
                    if (!self || self->is_closed()) return;
                    this->invalidate_mailbox_list(user_id, mailbox_id);
                    std::string response;
                    for (auto seq : expunged_seqs) {
                        response += "* " + std::to_string(seq) + " EXPUNGE\r\n";
                    }
                    response += tag + " OK EXPUNGE completed\r\n";

                    self->do_async_write(response, [](auto s, auto& ec) { if (!ec) s->do_async_read(); });
                    self->drain_buffered_commands();
                });
        });
}

// ---------- CLOSE ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_close(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    // 清除选中状态 + 回 CLOSE OK（回调里 session/ctx 由捕获保持存活）
    auto finish = [session, ctx, tag](std::function<void()> cont) {
        if (ctx) {
            ctx->mailbox_selected = false;
            ctx->selected_mailbox_name.clear();
            ctx->selected_mailbox_id = 0;
            ctx->read_only = false;
        }
        session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
        send_tagged(session, tag, "OK", "CLOSE completed");
        cont();
    };

    // If SELECTED, expunge deleted mails first（异步 CPS，完成后回包）
    if (static_cast<ImapState>(session->get_current_state()) == ImapState::SELECTED && ctx) {
        auto conn = this->acquire_connection(ctx->shard_index);
        if (conn->is_valid()) {
            session->set_paused(true);
            auto self = session->shared_from_this();
            uint64_t mailbox_id = ctx->selected_mailbox_id;
            uint64_t user_id = ctx->user_id;
            TraditionalImapsFsm<ConnectionType>::expunge_mailbox_async(
                conn, mailbox_id, user_id,
                [this, self, finish, mailbox_id, user_id](bool) {
                    if (!self || self->is_closed()) return;
                    this->invalidate_mailbox_list(user_id, mailbox_id);
                    finish([self]() { self->drain_buffered_commands(); });
                });
            return;
        }
    }
    finish([]() {});
}

// ---------- NOOP ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_noop(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";
    send_tagged(session, tag, "OK", "NOOP completed");
}

// ---------- CHECK ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_check(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";
    send_tagged(session, tag, "OK", "CHECK completed");
}

// ---------- STARTTLS ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_starttls(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    // STARTTLS only available on TCP connections (not already SSL)
    // Handoff logic: send OK, extract TCP socket, call server handoff
    session->do_async_write(tag + " OK Begin TLS negotiation now\r\n",
        [](std::shared_ptr<pr::SessionBase<ConnectionType>> self,
           const std::error_code& ec) mutable {
            if (ec) {
                LOG_IMAP_ERROR("Error sending STARTTLS response: {}", ec.message());
                return;
            }
            auto server = static_cast<ImapsServer*>(self->get_server());
            auto trace = self->take_trace_buffer();   // 交接给 TLS 会话，延续对话记录
            server->handoff_starttls(self->release_connection(), std::move(trace));
        }
    );
}

// ---------- CREATE ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_create(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated) {
        send_tagged(session, tag, "BAD", "Not authenticated");
        return;
    }

    std::string mailbox_name = session->get_last_command_args();
    if (!mailbox_name.empty() && mailbox_name[0] == '"') {
        size_t end = mailbox_name.find('"', 1);
        if (end != std::string::npos) {
            mailbox_name = mailbox_name.substr(1, end - 1);
        }
    }

    if (mailbox_name.empty()) {
        send_tagged(session, tag, "BAD", "CREATE requires mailbox name");
        return;
    }

    // 解码 IMAP-UTF-7 → UTF-8（客户端发来的名称可能是编码后的）
    mailbox_name = imap_utils::decode_mailbox_name(mailbox_name);

    auto conn = this->acquire_connection(ctx->shard_index);
    if (!conn->is_valid()) {
        send_tagged(session, tag, "NO", "Server error");
        return;
    }

    session->set_paused(true);
    (*conn)->async_execute(db::sql::build_imap_create_mailbox(),
                        {std::to_string(ctx->user_id), mailbox_name},
                        // ⚠ 回调必须持 conn：不持则 handle_* 返回即 ScopedConnection
                        // 析构归池，op 在飞时连接被他人借走（协议串号/UAF，见
                        // bench/imap REPORT 2026-09-24）。下同，全文件同规约。
                        [session, tag, conn](bool ok) {
                            if (ok) send_tagged(session, tag, "OK", "CREATE completed");
                            else send_tagged(session, tag, "NO", "CREATE failed (maybe already exists)");
                            session->drain_buffered_commands();
                        });
}

// ---------- DELETE ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_delete(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated) {
        send_tagged(session, tag, "BAD", "Not authenticated");
        return;
    }

    std::string mailbox_name = session->get_last_command_args();
    if (!mailbox_name.empty() && mailbox_name[0] == '"') {
        size_t end = mailbox_name.find('"', 1);
        if (end != std::string::npos) {
            mailbox_name = mailbox_name.substr(1, end - 1);
        }
    }

    auto conn = this->acquire_connection(ctx->shard_index);
    if (!conn->is_valid()) {
        send_tagged(session, tag, "NO", "Server error");
        return;
    }
    session->set_paused(true);
    auto self = session->shared_from_this();
    uint64_t user_id = ctx->user_id;

    // CPS 链：找邮箱 id → 系统邮箱检查 → 删消息 → 删邮箱
    TraditionalImapsFsm<ConnectionType>::find_mailbox_id_async(
        conn, user_id, mailbox_name,
        [this, self, conn, tag = std::move(tag), user_id](uint64_t mailbox_id) mutable {
            if (!self || self->is_closed()) return;
            if (mailbox_id == 0) {
                send_tagged(self, tag, "NO", "Mailbox not found");
                self->drain_buffered_commands();
                return;
            }

            // Check if it's a system mailbox
            (*conn)->async_query(db::sql::build_imap_check_mailbox_is_system(),
                                 {std::to_string(mailbox_id)},
                                 [this, self, conn, tag = std::move(tag), mailbox_id, user_id]
                                 (std::shared_ptr<IDBResult> result) mutable {
                                     if (result && result->get_row_count() > 0 &&
                                         result->get_value(0, "is_system") == "1") {
                                         send_tagged(self, tag, "NO", "Cannot delete system mailbox");
                                         self->drain_buffered_commands();
                                         return;
                                     }
                                     // 删除消息 + 删除邮箱
                                     (*conn)->async_execute(db::sql::build_imap_delete_mailbox_messages(),
                                                            {std::to_string(mailbox_id)},
                                                            [this, self, conn, tag = std::move(tag), mailbox_id, user_id]
                                                            (bool ok1) mutable {
                                                                (*conn)->async_execute(
                                                                    db::sql::build_imap_delete_mailbox(),
                                                                    {std::to_string(mailbox_id)},
                                                                    [this, self, conn, tag = std::move(tag), ok1, mailbox_id, user_id](bool ok2) {
                                                                        this->invalidate_mailbox_list(user_id, mailbox_id);
                                                                        if (ok1 && ok2)
                                                                            send_tagged(self, tag, "OK", "DELETE completed");
                                                                        else
                                                                            send_tagged(self, tag, "NO", "DELETE failed");
                                                                        self->drain_buffered_commands();
                                                                    });
                                                              });
                                 });
        });
}

// ---------- RENAME ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_rename(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated) {
        send_tagged(session, tag, "BAD", "Not authenticated");
        return;
    }

    // Parse old_name new_name
    std::string old_name, new_name, args = session->get_last_command_args();
    size_t space = args.find(' ');
    if (space != std::string::npos) {
        old_name = args.substr(0, space);
        new_name = args.substr(space + 1);
    }

    // Trim quotes
    auto trim_quotes = [](std::string& s) {
        if (!s.empty() && s[0] == '"') {
            size_t end = s.find('"', 1);
            if (end != std::string::npos) s = s.substr(1, end - 1);
        }
    };
    trim_quotes(old_name);
    trim_quotes(new_name);

    if (old_name.empty() || new_name.empty()) {
        send_tagged(session, tag, "BAD", "RENAME requires old and new names");
        return;
    }

    // 新名称也要解码（客户端发来的可能是 IMAP-UTF-7 编码）
    new_name = imap_utils::decode_mailbox_name(new_name);

    auto conn = this->acquire_connection(ctx->shard_index);
    if (!conn->is_valid()) {
        send_tagged(session, tag, "NO", "Server error");
        return;
    }
    session->set_paused(true);
    auto self = session->shared_from_this();
    uint64_t user_id = ctx->user_id;

    // CPS 链：找邮箱 id → 改名
    TraditionalImapsFsm<ConnectionType>::find_mailbox_id_async(
        conn, user_id, old_name,
        [self, conn, tag = std::move(tag), new_name = std::move(new_name)](
            uint64_t mailbox_id) mutable {
            if (!self || self->is_closed()) return;
            if (mailbox_id == 0) {
                send_tagged(self, tag, "NO", "Mailbox not found");
                self->drain_buffered_commands();
                return;
            }
            (*conn)->async_execute(db::sql::build_imap_rename_mailbox(),
                                   {new_name, std::to_string(mailbox_id)},
                                   [self, conn, tag = std::move(tag)](bool ok) {
                                       if (ok) send_tagged(self, tag, "OK", "RENAME completed");
                                       else send_tagged(self, tag, "NO", "RENAME failed");
                                       self->drain_buffered_commands();
                                   });
        });
}

// ---------- SUBSCRIBE ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_subscribe(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";
    LOG_IMAP_INFO("SUBSCRIBE {} — subscription persistence not yet implemented");
    send_tagged(session, tag, "OK", "SUBSCRIBE completed");
}

// ---------- UNSUBSCRIBE ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_unsubscribe(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";
    LOG_IMAP_INFO("UNSUBSCRIBE {} — subscription persistence not yet implemented");
    send_tagged(session, tag, "OK", "UNSUBSCRIBE completed");
}

// ---------- APPEND ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_append(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated) {
        send_tagged(session, tag, "BAD", "Not authenticated");
        return;
    }

    if (ctx->pending_append_preamble.empty()) {
        send_tagged(session, tag, "BAD", "APPEND missing preamble");
        return;
    }

    // 解析 preamble: mailbox (flags) "InternalDate"
    std::string preamble = ctx->pending_append_preamble;
    ctx->pending_append_preamble.clear();   // 消费即清：防串到下一条 APPEND
    std::string mailbox_name;
    std::string flags_str;

    // 提取邮箱名（可能带引号）
    if (preamble[0] == '"') {
        size_t end = preamble.find('"', 1);
        if (end != std::string::npos) {
            mailbox_name = preamble.substr(1, end - 1);
            std::string rest = preamble.substr(end + 1);
            // trim leading spaces
            rest.erase(0, rest.find_first_not_of(" \t"));
            if (!rest.empty() && rest[0] == '(') {
                size_t paren = rest.find(')');
                if (paren != std::string::npos) {
                    flags_str = rest.substr(0, paren + 1);
                }
            }
        }
    } else {
        size_t sp = preamble.find(' ');
        if (sp != std::string::npos) {
            mailbox_name = preamble.substr(0, sp);
            std::string rest = preamble.substr(sp + 1);
            rest.erase(0, rest.find_first_not_of(" \t"));
            if (!rest.empty() && rest[0] == '(') {
                size_t paren = rest.find(')');
                if (paren != std::string::npos) {
                    flags_str = rest.substr(0, paren + 1);
                }
            }
        } else {
            mailbox_name = preamble;
        }
    }

    // 解析 flags
    int init_status = 0; // 0=read
    if (flags_str.find("\\Seen") != std::string::npos || flags_str.find("\\seen") != std::string::npos) {
        init_status = 0;
    } else if (flags_str.find("\\Unseen") != std::string::npos || flags_str.find("\\Draft") != std::string::npos) {
        init_status = 1; // unread / draft
    }
    if (flags_str.find("\\Deleted") != std::string::npos) {
        init_status = 5; // deleted
    }

    // 解码邮箱名（IMAP-UTF-7 → UTF-8）
    mailbox_name = imap_utils::decode_mailbox_name(mailbox_name);

    // 正文内容即 literal 数据（args）
    std::string body_content = session->get_last_command_args();
    std::string subject = "(APPEND)";

    // 尝试提取 Subject（正文第一行或全文首 200 字符）
    if (!body_content.empty()) {
        std::istringstream ss(body_content);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.find("Subject:") == 0 || line.find("subject:") == 0) {
                subject = line.substr(8);
                // trim
                subject.erase(0, subject.find_first_not_of(" \t"));
                break;
            }
        }
    }

    // CPS 链：找目标邮箱 → worker 写 storage + DB（阻塞 I/O 不进 io 线程）→
    // create → user_email → link → 回 APPENDUID
    auto conn = this->acquire_connection(ctx->shard_index);
    if (!conn->is_valid()) {
        send_tagged(session, tag, "NO", "Server database unavailable");
        return;
    }
    session->set_paused(true);
    auto self = session->shared_from_this();
    uint64_t user_id = ctx->user_id;

    TraditionalImapsFsm<ConnectionType>::find_mailbox_id_async(
        conn, user_id, mailbox_name,
        [this, self, conn, tag = std::move(tag), subject = std::move(subject),
         body_content = std::move(body_content), init_status,
         mailbox_name = mailbox_name, user_id](
            uint64_t target_mbox_id) mutable {
            if (!self || self->is_closed()) return;
            if (target_mbox_id == 0) {
                send_tagged(self, tag, "NO", "APPEND failed: mailbox not found");
                self->drain_buffered_commands();
                return;
            }
            auto storage = this->get_storage(0);
            auto worker = this->m_workerThreadPool;
            if (!worker) {
                send_tagged(self, tag, "NO", "Server not ready");
                self->drain_buffered_commands();
                return;
            }
            // storage append 是阻塞 I/O，整体丢 worker
            worker->post([this, self, conn, tag = std::move(tag), storage,
                          subject = std::move(subject), body_content = std::move(body_content),
                          init_status, mailbox_name = std::move(mailbox_name),
                          user_id, target_mbox_id]() mutable {
                TraditionalImapsFsm<ConnectionType>::create_mail_async(
                    storage, conn, subject, body_content,
                    [this, self, conn, storage, tag = std::move(tag), user_id, target_mbox_id,
                     init_status, mailbox_name = std::move(mailbox_name)](
                        uint64_t mail_id, std::string, std::string error) mutable {
                        if (!self || self->is_closed()) return;
                        if (mail_id == 0) {
                            send_tagged(self, tag, "NO", "[SERVERBUG] APPEND failed: " + error);
                            self->drain_buffered_commands();
                            return;
                        }
                        TraditionalImapsFsm<ConnectionType>::get_user_email_async(
                            conn, user_id,
                            [this, self, conn, storage, tag = std::move(tag), mail_id, user_id,
                             target_mbox_id, init_status, mailbox_name = std::move(mailbox_name)](
                                std::string user_email) mutable {
                                if (!self || self->is_closed()) return;
                                if (user_email.empty()) {
                                    auto* c = static_cast<ImapContext*>(self->get_context());
                                    if (c) user_email = c->username;
                                }
                                TraditionalImapsFsm<ConnectionType>::link_mail_to_mailbox_async(
                                    conn, mail_id, user_id, target_mbox_id,
                                    user_email, user_email, init_status,
                                    [this, self, conn, storage, tag = std::move(tag), mail_id,
                                     target_mbox_id, user_id, mailbox_name = std::move(mailbox_name),
                                     user_email](bool ok) mutable {
                                        if (!self || self->is_closed()) return;
                                        if (!ok) {
                                            // 关联写失败 → 回 tagged NO，并清掉半写状态：
                                            // mails/mail_recipients/mail_mailbox 三表显式删
                                            // （生产 FK 不明，不赌级联），体文件一并删除。
                                            // 之前忽略失败仍回 OK，且孤儿体文件永留盘上
                                            // （2026-10-21 生产实测 /opt/smtpServer/mail/<id>）。
                                            LOG_IMAP_ERROR(
                                                "APPEND: link failed for mail_id={}, rolling back",
                                                mail_id);
                                            TraditionalImapsFsm<ConnectionType>::delete_mail_cascade_async(
                                                conn, storage, mail_id,
                                                [self, tag = std::move(tag)](bool removed) {
                                                    if (!self || self->is_closed()) return;
                                                    if (!removed)
                                                        LOG_IMAP_ERROR(
                                                            "APPEND: rollback incomplete, orphan may remain");
                                                    send_tagged(self, tag, "NO",
                                                                "[SERVERBUG] APPEND failed");
                                                    self->drain_buffered_commands();
                                                });
                                            return;
                                        }
                                        this->invalidate_mailbox_list(user_id, target_mbox_id);
                                        // 返回 APPENDUID
                                        uint64_t uidvalidity = target_mbox_id;
                                        std::string response = tag + " OK [APPENDUID "
                                            + std::to_string(uidvalidity) + " "
                                            + std::to_string(mail_id) + "] APPEND completed\r\n";
                                        self->do_async_write(response, [](auto s, auto& ec) { if (!ec) s->do_async_read(); });
                                        self->drain_buffered_commands();
                                        LOG_IMAP_INFO("APPEND: mail_id={}, mailbox={}, user={}",
                                                      mail_id, mailbox_name, user_email);
                                    });
                            });
                    });
            });
        });
}

// ---------- SEARCH ----------

namespace {
// SEARCH 判据用的轻量分词：按空白切，双引号串为一个 token（\" 转义）。
std::vector<std::string> imap_search_tokens(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        if (i >= s.size()) break;
        std::string tok;
        if (s[i] == '"') {
            ++i;
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < s.size()) ++i;
                tok += s[i++];
            }
            if (i < s.size()) ++i;   // 收尾引号
        } else {
            while (i < s.size() && s[i] != ' ' && s[i] != '\t') tok += s[i++];
        }
        out.push_back(std::move(tok)); // 空引号串也是 HEADER 的合法 value
    }
    return out;
}

struct ImapSearchHeaderKey {
    std::string field;   // 头字段名（原始大小写，匹配时忽略大小写）
    std::string value;   // 子串（匹配时忽略大小写）
};

struct ImapSearchCriteria {
    std::vector<ImapSearchHeaderKey> headers;
    std::vector<std::string> keys; // 已消费 HEADER 参数，剩余的 flag/UID 判据（大写）
};

// 一次消费 HEADER <field> <value>，其参数不能再被识别为 flag 或 UID 范围。
ImapSearchCriteria parse_search_criteria(const std::string& args) {
    ImapSearchCriteria criteria;
    auto tokens = imap_search_tokens(args);
    for (size_t i = 0; i < tokens.size(); ++i) {
        std::string key = tokens[i];
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (key == "HEADER") {
            if (i + 2 >= tokens.size()) break;
            criteria.headers.push_back({tokens[i + 1], tokens[i + 2]});
            i += 2;
        } else {
            criteria.keys.push_back(std::move(key));
        }
    }
    return criteria;
}

// 头字段抽取：header 块内所有同名行（含 folded 续行）合并为一个字符串。
// 找不到字段返回 false。字段名与内容的大小写都不参与（匹配在调用方做不敏感化）。
bool collect_header_field(const std::string& header_block,
                          const std::string& field_name, std::string& out) {
    auto lower = [](std::string v) {
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return v;
    };
    const std::string want = lower(field_name);
    bool found = false;
    bool selected = false;
    size_t pos = 0;
    while (pos < header_block.size()) {
        size_t eol = header_block.find('\n', pos);
        if (eol == std::string::npos) eol = header_block.size();
        std::string line = header_block.substr(pos, eol - pos);
        pos = eol == header_block.size() ? header_block.size() : eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;   // 头块结束
        if (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            if (selected) { out += " "; out += mime_params_detail::trim(line); }
            continue;
        }
        selected = false;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        if (lower(mime_params_detail::trim(line.substr(0, colon))) == want) {
            selected = true;
            found = true;
            out += mime_params_detail::trim(line.substr(colon + 1));
        }
    }
    return found;
}

// 邮件体正文文件读取（SEARCH HEADER 用；storage 缺席时退回本地 ifstream）
std::string search_read_body(const std::shared_ptr<storage::IStorageProvider>& storage,
                             const std::string& body_path) {
    if (body_path.empty()) return {};
    if (storage) {
        storage::IoError err;
        std::string data;
        if (storage->read_all(body_path, data, err)) return data;
    }
    std::ifstream in(body_path, std::ios::binary);
    if (!in.is_open()) return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// HEADER 判据匹配：仅在该头字段内做大小写不敏感子串匹配；
// 字段不存在 → 不命中；value 为空 → 有该字段即命中（RFC 3501 §6.4.4）。
bool mail_matches_header_keys(const std::shared_ptr<storage::IStorageProvider>& storage,
                              const MailboxMailInfo& m,
                              const std::vector<ImapSearchHeaderKey>& keys) {
    // 旧版 GCC APPEND 把已移动的 body_path 写进 DB。只读兼容这些历史行，
    // 用与 APPEND 相同的 provider key 寻址；不在 SEARCH 中修写生产数据。
    const std::string body_path = !m.body_path.empty() ? m.body_path
        : storage ? storage->build_mail_body_key(m.mail_id) : "mail/" + std::to_string(m.mail_id);
    const std::string body = search_read_body(storage, body_path);
    size_t sep = body.find("\r\n\r\n");
    if (sep == std::string::npos) sep = body.find("\n\n");
    const std::string header_block =
        (sep == std::string::npos) ? body : body.substr(0, sep);
    auto lower = [](std::string v) {
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return v;
    };
    for (const auto& key : keys) {
        std::string field_text;
        if (!collect_header_field(header_block, key.field, field_text)) return false;
        if (key.value.empty()) continue;
        if (lower(field_text).find(lower(key.value)) == std::string::npos) return false;
    }
    return true;
}
} // namespace

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_search(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session, bool is_uid)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated || !ctx->mailbox_selected) {
        send_tagged(session, tag, "BAD", "No mailbox selected");
        return;
    }

    const std::string raw_args = ctx->is_uid_command
        ? ctx->uid_overridden_args : session->get_last_command_args();
    const auto criteria = parse_search_criteria(raw_args);
    const auto& header_keys = criteria.headers;
    const auto& tokens = criteria.keys;
    auto has_token = [&](const char* t) {
        for (const auto& tok : tokens)
            if (tok == t) return true;
        return false;
    };
    const bool search_unseen = has_token("UNSEEN") || has_token("NEW");
    const bool search_seen = has_token("SEEN") && !has_token("UNSEEN");
    const bool search_deleted = has_token("DELETED") && !has_token("UNDELETED");

    auto conn = this->acquire_connection(ctx->shard_index);
    if (!conn->is_valid()) {
        send_tagged(session, tag, "NO", "Server database unavailable");
        return;
    }
    session->set_paused(true);
    auto self = session->shared_from_this();
    uint64_t mailbox_id = ctx->selected_mailbox_id;
    uint64_t user_id = ctx->user_id;
    auto storage = this->get_storage(0);
    auto worker = this->m_workerThreadPool;

    TraditionalImapsFsm<ConnectionType>::get_mailbox_mails_async(
        conn, mailbox_id, user_id,
        [self, tag = std::move(tag), is_uid,
         search_unseen, search_seen, search_deleted,
         header_keys, storage, worker,
         tokens](
            bool ok, std::vector<MailboxMailInfo> mails) mutable {
            if (!self || self->is_closed()) return;
            if (!ok) {
                send_tagged(self, tag, "NO", "Server error");
                self->drain_buffered_commands();
                return;
            }

            // UID 范围条件（如 "UID SEARCH UID 2083...:*"）：只返回 UID 落在该区间的邮件。
            // 之前完全忽略此条件，导致 "UID SEARCH UID <last>:*" 总是返回整箱，
            // 客户端"最后同步 UID"永远无法推进 → 每次轮询都全量重同步。
            bool has_uid_range = false;
            uint64_t uid_lo = 0, uid_hi = UINT64_MAX;
            for (size_t i = 0; i + 1 < tokens.size(); ++i) {
                if (tokens[i] != "UID") continue;
                const auto& tok = tokens[i + 1];
                if (tok.empty() || (tok[0] != '*' &&
                    !std::isdigit(static_cast<unsigned char>(tok[0])))) continue;
                size_t colon = tok.find(':');
                if (colon != std::string::npos) {
                    std::string a = tok.substr(0, colon), b = tok.substr(colon + 1);
                    uid_lo = (a == "*") ? 0 : safe_stoull(a);
                    uid_hi = (b == "*") ? UINT64_MAX : safe_stoull(b);
                } else if (tok != "*") {
                    uid_lo = uid_hi = safe_stoull(tok);
                }
                has_uid_range = true;
                break;
            }

            // 匹配谓词：flag/UID 判据与旧实现完全一致，HEADER 判据在末尾 AND。
            // 显式按值捕获：worker 路径要把它整体搬进 post 的 lambda，
            // 引用捕获会在回调返回后悬空。
            auto matches = [search_unseen, search_seen, search_deleted, has_uid_range,
                            uid_lo, uid_hi, header_keys, storage](const MailboxMailInfo& m) {
                bool match = true;
                if (search_unseen) match = (m.status == 1);
                else if (search_seen) match = (m.status == 0);
                if (search_deleted) match = m.is_deleted;
                if (has_uid_range)
                    match = match && (m.mail_id >= uid_lo && m.mail_id <= uid_hi);
                if (match && !header_keys.empty())
                    match = match && mail_matches_header_keys(storage, m, header_keys);
                return match;
            };

            // HEADER 判据要逐封读体文件（阻塞 I/O），整段丢 worker 跑；
            // 纯 flag 判据维持原内联路径（Leger 的 UNSEEN 轮询不受影响）。
            if (!header_keys.empty() && worker) {
                worker->post([self, tag = std::move(tag), is_uid,
                              mails = std::move(mails), matches]() mutable {
                    std::string response = "* SEARCH";
                    for (size_t i = 0; i < mails.size(); ++i) {
                        if (matches(mails[i])) {
                            // UID SEARCH 返回 mail_id，普通 SEARCH 返回 seq number
                            response += " " + std::to_string(is_uid ? mails[i].mail_id : (i + 1));
                        }
                    }
                    response += "\r\n";
                    response += tag + " OK SEARCH completed\r\n";
                    self->do_async_write(response,
                        [](auto s, auto& ec) { if (!ec) s->do_async_read(); });
                    self->drain_buffered_commands();
                });
                return;
            }

            std::string response = "* SEARCH";
            for (size_t i = 0; i < mails.size(); ++i) {
                if (matches(mails[i])) {
                    // UID SEARCH 返回 mail_id，普通 SEARCH 返回 seq number
                    response += " " + std::to_string(is_uid ? mails[i].mail_id : (i + 1));
                }
            }
            response += "\r\n";
            response += tag + " OK SEARCH completed\r\n";

            self->do_async_write(response, [](auto s, auto& ec) { if (!ec) s->do_async_read(); });
            self->drain_buffered_commands();
        });
}

// ---------- UID ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_uid(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated || !ctx->mailbox_selected) {
        send_tagged(session, tag, "BAD", "No mailbox selected");
        return;
    }

    std::string args = session->get_last_command_args();
    size_t space = args.find(' ');
    if (space == std::string::npos) {
        send_tagged(session, tag, "BAD", "UID requires subcommand");
        return;
    }

    std::string subcmd = args.substr(0, space);
    std::string subargs = args.substr(space + 1);
    std::transform(subcmd.begin(), subcmd.end(), subcmd.begin(), ::toupper);

    LOG_IMAP_INFO("UID subcmd={} args={}", subcmd, subargs);

    // UID FETCH/STORE/COPY: 把 UID 序列号映射为 mails 数组下标+1（CPS 查邮件列表）
    if (subcmd == "FETCH" || subcmd == "STORE" || subcmd == "COPY") {
        auto conn = this->acquire_connection(ctx->shard_index);
        if (!conn->is_valid()) {
            send_tagged(session, tag, "NO", "Server database unavailable");
            return;
        }
        session->set_paused(true);
        auto self = session->shared_from_this();
        uint64_t mailbox_id = ctx->selected_mailbox_id;
        uint64_t user_id = ctx->user_id;
        std::string scmd = subcmd;

        TraditionalImapsFsm<ConnectionType>::get_mailbox_mails_async(
            conn, mailbox_id, user_id,
            [this, self, tag = std::move(tag), scmd = std::move(scmd),
             subargs = std::move(subargs)](bool ok,
                 std::vector<MailboxMailInfo> mails) mutable {
                if (!self || self->is_closed()) return;
                if (!ok || mails.empty()) {
                    send_tagged(self, tag, "OK", scmd + " completed (empty)");
                    self->drain_buffered_commands();
                    return;
                }
                // 构建 UID → seq 映射
                std::unordered_map<uint64_t, uint64_t> uid_to_seq;
                for (size_t i = 0; i < mails.size(); ++i)
                    uid_to_seq[mails[i].mail_id] = i + 1;

                // 找到 args 中的空格（分割 序列号 和 属性）
                size_t sp = subargs.find(' ');
                std::string uid_set = (sp != std::string::npos) ? subargs.substr(0, sp) : subargs;
                std::string rest    = (sp != std::string::npos) ? subargs.substr(sp) : "";

                // UID set → seq set
                std::string seq_set;
                if (uid_set.find(',') != std::string::npos) {
                    // 逗号分隔的 UID 列表，逐个映射为 seq（保持原顺序）
                    std::istringstream us(uid_set);
                    std::string tok;
                    while (std::getline(us, tok, ',')) {
                        if (tok.empty()) continue;
                        auto it = uid_to_seq.find(safe_stoull(tok));
                        uint64_t s = (it != uid_to_seq.end()) ? it->second : 0;
                        if (s > 0) {
                            if (!seq_set.empty()) seq_set += ",";
                            seq_set += std::to_string(s);
                        }
                    }
                    if (seq_set.empty()) seq_set = "0";
                } else if (uid_set.find(':') != std::string::npos) {
                    // UID 范围：>= start 且 <= end（* 表示无穷）。
                    // 之前要求起止 UID 精确匹配某封邮件，客户端发 "UID <last>:*" 且 <last>
                    // 不是确切 UID 时映射成 0:* → 全量拉取，导致客户端永远重拉。
                    size_t c = uid_set.find(':');
                    std::string u1 = uid_set.substr(0, c), u2 = uid_set.substr(c + 1);
                    uint64_t start_uid = (u1 == "*") ? 0 : safe_stoull(u1);
                    uint64_t end_uid = (u2 == "*") ? UINT64_MAX : safe_stoull(u2);
                    std::string collected;
                    for (size_t i = 0; i < mails.size(); ++i) {
                        if (mails[i].mail_id >= start_uid && mails[i].mail_id <= end_uid) {
                            if (!collected.empty()) collected += ",";
                            collected += std::to_string(i + 1);
                        }
                    }
                    seq_set = collected.empty() ? "0" : collected;
                } else if (uid_set == "*") {
                    seq_set = "*";
                } else {
                    auto it = uid_to_seq.find(safe_stoull(uid_set));
                    seq_set = std::to_string(it != uid_to_seq.end() ? it->second : 0);
                }
                std::string mapped_args = seq_set + rest;

                auto* c = static_cast<ImapContext*>(self->get_context());
                c->is_uid_command = true;
                c->uid_overridden_args = mapped_args;

                if (scmd == "FETCH") {
                    handle_fetch(self, true);
                } else if (scmd == "STORE") {
                    handle_store(self);
                } else if (scmd == "COPY") {
                    handle_copy(self);
                } else {
                    send_tagged(self, tag, "BAD", "Unknown UID subcommand");
                    self->drain_buffered_commands();
                }

                c->is_uid_command = false;
                c->uid_overridden_args.clear();
                LOG_IMAP_INFO("UID subcmd done: {}", scmd);
            });
        return;
    }

    ctx->is_uid_command = true;
    ctx->uid_overridden_args = subargs;

    if (subcmd == "SEARCH") {
        handle_search(session, true);
    } else {
        send_tagged(session, tag, "BAD", "Unknown UID subcommand");
    }

    ctx->is_uid_command = false;
    ctx->uid_overridden_args.clear();
    LOG_IMAP_INFO("UID subcmd done: {}", subcmd);
}

// ---------- COPY / MOVE (共享实现) ----------
// 把当前选中的邮箱中的一组邮件复制/移动到目标邮箱
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_copy_move(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session,
    bool is_move)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated || !ctx->mailbox_selected) {
        send_tagged(session, tag, "BAD", "No mailbox selected");
        return;
    }

    // 解析 "seq_set mailbox"
    auto args = (ctx && ctx->is_uid_command) ? ctx->uid_overridden_args : session->get_last_command_args();
    size_t sp = args.rfind(' ');
    if (sp == std::string::npos) {
        send_tagged(session, tag, "BAD", "COPY/MOVE requires sequence set and mailbox");
        return;
    }

    std::string seq_set = args.substr(0, sp);
    std::string target_name = args.substr(sp + 1);
    // trim quotes
    if (!target_name.empty() && target_name[0] == '"') {
        size_t end = target_name.find('"', 1);
        if (end != std::string::npos) target_name = target_name.substr(1, end - 1);
    }
    target_name = imap_utils::decode_mailbox_name(target_name);

    auto conn = this->acquire_connection(ctx->shard_index);
    if (!conn->is_valid()) {
        send_tagged(session, tag, "NO", "Server error");
        return;
    }
    session->set_paused(true);
    auto self = session->shared_from_this();
    uint64_t user_id = ctx->user_id;
    uint64_t source_mailbox_id = ctx->selected_mailbox_id;

    // CPS 链：找目标邮箱 → 源邮箱邮件列表 → 批量 INSERT IGNORE（单条 VALUES 多行）
    // → MOVE 再批量标记源邮箱 is_deleted=1
    TraditionalImapsFsm<ConnectionType>::find_mailbox_id_async(
        conn, user_id, target_name,
        [this, self, conn, tag = std::move(tag), seq_set = std::move(seq_set),
         is_move, user_id, source_mailbox_id](uint64_t target_id) mutable {
            if (!self || self->is_closed()) return;
            if (target_id == 0) {
                send_tagged(self, tag, "NO", "COPY/MOVE failed: mailbox not found");
                self->drain_buffered_commands();
                return;
            }

            TraditionalImapsFsm<ConnectionType>::get_mailbox_mails_async(
                conn, source_mailbox_id, user_id,
                [this, self, conn, tag = std::move(tag), seq_set = std::move(seq_set),
                 is_move, user_id, source_mailbox_id, target_id](
                    bool ok, std::vector<MailboxMailInfo> mails) mutable {
                    if (!self || self->is_closed()) return;
                    if (!ok) {
                        send_tagged(self, tag, "NO", "Server error");
                        self->drain_buffered_commands();
                        return;
                    }

                    // 建立序号→mail_id 映射
                    std::vector<uint64_t> mail_ids;
                    if (seq_set == "*") {
                        for (const auto& m : mails) mail_ids.push_back(m.mail_id);
                    } else if (seq_set.find(':') != std::string::npos) {
                        size_t colon = seq_set.find(':');
                        uint64_t start = safe_stoull(seq_set.substr(0, colon));
                        uint64_t end_val = safe_stoull(seq_set.substr(colon + 1));
                        if (end_val > mails.size()) end_val = mails.size();
                        for (uint64_t s = start; s <= end_val && s <= mails.size(); ++s) {
                            mail_ids.push_back(mails[s - 1].mail_id);
                        }
                    } else {
                        uint64_t n = safe_stoull(seq_set);
                        if (n >= 1 && n <= mails.size())
                            mail_ids.push_back(mails[n - 1].mail_id);
                    }

                    if (mail_ids.empty()) {
                        send_tagged(self, tag, "OK", "COPY/MOVE completed (no messages)");
                        self->drain_buffered_commands();
                        return;
                    }

                    // 批量插入：INSERT IGNORE 单条 VALUES 多行 + ROW_COUNT 计实际插入
                    TraditionalImapsFsm<ConnectionType>::batch_insert_mailbox_async(
                        conn, mail_ids, target_id, user_id,
                        [this, self, conn, tag = std::move(tag), is_move,
                         user_id, source_mailbox_id, target_id, mail_ids = std::move(mail_ids)](
                            size_t copied) mutable {
                            if (!self || self->is_closed()) return;
                            // 目标邮箱新增行（COPY 的 mail_id 可能低于目标现有最大值，
                            // uidnext 不推进，TTL 前缓存不会自愈）；MOVE 还动了源邮箱
                            this->invalidate_mailbox_list(user_id, target_id);
                            if (is_move) this->invalidate_mailbox_list(user_id, source_mailbox_id);
                            auto reply = [self, tag = std::move(tag), is_move, copied]() mutable {
                                std::string cmd_name = is_move ? "MOVE" : "COPY";
                                std::string response = tag + " OK " + cmd_name
                                    + " completed (" + std::to_string(copied) + " messages)\r\n";
                                self->do_async_write(response, [](auto s, auto& ec) { if (!ec) s->do_async_read(); });
                                self->drain_buffered_commands();
                            };
                            if (is_move) {
                                // MOVE: 源邮箱标记 is_deleted=1（expunge 时物理删）
                                TraditionalImapsFsm<ConnectionType>::batch_mark_move_deleted_async(
                                    conn, mail_ids, user_id, source_mailbox_id,
                                    [reply = std::move(reply)](bool) mutable { reply(); });
                            } else {
                                reply();
                            }
                        });
                });
        });
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_copy(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session) {
    handle_copy_move(session, false);
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_move(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session) {
    handle_copy_move(session, true);
}

// ---------- IDLE ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_idle(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx) {
        send_tagged(session, tag, "BAD", "Not authenticated");
        return;
    }

    ctx->idle_mode = true;

    // IDLE 是合法的长无数据等待态：撤掉看门狗，避免被闲置超时误回收；
    // DONE 到达时 handle_done 里重新 arm。
    session->disarm_timeout();

    // RFC 2177: send continuation
    session->do_async_write("+ idling\r\n",
        [](std::shared_ptr<pr::SessionBase<ConnectionType>> s,
           const std::error_code& ec) mutable {
            if (ec) {
                LOG_IMAP_ERROR("Error sending IDLE continuation: {}", ec.message());
                return;
            }
            // Continue reading — DONE command will be sent by client
            // NOTE: IDLE push notifications require a background polling
            // mechanism that is beyond the current scope. The IMAP server
            // accepts IDLE/DONE correctly; new-mail notifications (untagged
            // EXISTS) will be sent when the client exits IDLE and issues
            // a NOOP or re-selects the mailbox.
            s->do_async_read();
        }
    );
}

// ---------- DONE ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_done(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    if (!ctx) return;

    ctx->idle_mode = false;
    session->rearm(session->config_read_timeout());   // 离开 IDLE，重新 arm

    std::string tag = ctx->current_tag;
    send_tagged(session, tag, "OK", "IDLE terminated");
}

// ---------- ERROR ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_error(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";
    auto err = session->get_last_command_args();
    send_tagged(session, tag, "BAD", err.empty() ? "Protocol error" : err);
}

// ---------- TIMEOUT ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_timeout(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    session->do_async_write("* BYE TIMEOUT\r\n",
        [](std::shared_ptr<pr::SessionBase<ConnectionType>> s,
           const std::error_code& ec) mutable {
            if (ec) {
                LOG_IMAP_ERROR("Error sending TIMEOUT BYE: {}", ec.message());
                return;
            }
            s->close();
        }
    );
}

// ========== 辅助函数 ==========

// ====================================================================
// ImapsFsm DB helper —— CPS：async_query/async_execute 链
// ====================================================================
// 全部 static：可在 worker/异步回调里调用。底层 MySQL async_* 目前是默认
// 同步包装（回调同步触发），但调用方结构已异步就绪；将来接真异步 DB 时
// 这些调用方无需改动。conn 由调用方持有的 shared ScopedConnection 保活。

// 用户认证（仿 POP3 auth_user_async / SMTP auth 链）。调用方须在 worker
// 线程发起（bcrypt 几十~几百 ms 纯 CPU，不许在 io 线程算）。
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::auth_user_async(
    const std::shared_ptr<router::IShardRouter>& shard_router,
    const std::shared_ptr<AuthCache>& auth_cache,
    const std::string& mail_address,
    const std::string& password,
    std::function<void(bool, uint64_t, int)> cb)
{
    if (!cb) return;
    LOG_AUTH_INFO("IMAP AUTH attempt: mail_address=[{}]", mail_address);

    int shard = 0;
    if (shard_router) {
        int r = shard_router->route(mail_address);
        if (r >= 0) shard = r;
    }

    // 快路径：缓存命中（含 status!=1 的负缓存）同步回调
    AuthCacheEntry ce;
    if (auth_cache && auth_cache->lookup(mail_address, ce)) {
        if (ce.status != 1) { cb(false, 0, shard); return; }
        bool ok = verify_password_hash(password, ce.password_hash);
        cb(ok, ce.user_id, ce.shard);
        return;
    }

    if (!shard_router) { cb(false, 0, shard); return; }
    auto db_pool = shard_router->get_db_pool(static_cast<size_t>(shard));
    if (!db_pool) {
        LOG_AUTH_ERROR("No database pool for shard {}", shard);
        cb(false, 0, shard);
        return;
    }
    // conn 用 shared 保活：真异步接入后回调在 DB 线程触发，链中捕获不悬垂
    auto conn = db_pool->acquire_connection();
    if (!conn->is_valid()) {
        LOG_AUTH_ERROR("Failed to get database connection for shard {}", shard);
        cb(false, 0, shard);
        return;
    }

    (*conn)->async_query(db::sql::build_auth_user_query(), {mail_address},
        [auth_cache, mail_address, password, shard, conn,
         cb = std::move(cb)](std::shared_ptr<IDBResult> result) mutable {
            if (!result || result->get_row_count() == 0) {
                LOG_AUTH_WARN("User not found: {}", mail_address);
                cb(false, 0, shard);
                return;
            }
            int status = static_cast<int>(safe_stoull(result->get_value(0, "status")));
            if (status != 1) {
                LOG_AUTH_WARN("User account disabled: {}", mail_address);
                cb(false, 0, shard);
                return;
            }
            std::string stored = result->get_value(0, "password");
            uint64_t user_id = safe_stoull(result->get_value(0, "id"));
            AuthCacheEntry entry{stored, status, user_id, shard};
            if (auth_cache) auth_cache->store(mail_address, entry);
            if (!verify_password_hash(password, stored)) { cb(false, 0, shard); return; }
            // 验密通过：MD5/明文历史行 → bcrypt 回写 + 刷缓存（已是 bcrypt 则直通），
            // 再记 last_login，最后回调。见 common/password_rehash.h 的契约说明。
            rehash_password_if_needed_async(conn, auth_cache, mail_address, password, stored, entry,
                [conn, mail_address, user_id, shard, cb = std::move(cb)]() mutable {
                    (*conn)->async_execute(db::sql::build_update_last_login(), {mail_address},
                        [conn, cb = std::move(cb), user_id, shard](bool) { cb(true, user_id, shard); });
                });
        });
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::get_mailboxes_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t user_id,
    std::function<void(bool, std::vector<std::tuple<uint64_t, std::string, int>>)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(false, {}); return; }
    (*conn)->async_query(db::sql::build_imap_list_mailboxes(), {std::to_string(user_id)},
        [conn, cb = std::move(cb)](std::shared_ptr<IDBResult> result) mutable {
            std::vector<std::tuple<uint64_t, std::string, int>> mailboxes;
            if (!result) { cb(false, std::move(mailboxes)); return; }
            for (size_t i = 0; i < result->get_row_count(); ++i) {
                uint64_t id = safe_stoull(result->get_value(i, "id"));
                std::string name = result->get_value(i, "name");
                int box_type = static_cast<int>(safe_stoull(result->get_value(i, "box_type")));
                mailboxes.emplace_back(id, name, box_type);
            }
            cb(true, std::move(mailboxes));
        });
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::find_mailbox_id_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t user_id,
    const std::string& mailbox_name, std::function<void(uint64_t)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(0); return; }
    std::string name_utf8 = mailbox_name;
    if (mailbox_name.find('&') != std::string::npos) {
        std::string decoded = decode_imap_utf7(mailbox_name);
        if (!decoded.empty()) name_utf8 = decoded;
    }
    (*conn)->async_query(db::sql::build_imap_get_mailbox_by_name(),
        {std::to_string(user_id), name_utf8},
        [conn, user_id, name_utf8, cb = std::move(cb)](std::shared_ptr<IDBResult> result) mutable {
            if (result && result->get_row_count() > 0) {
                cb(safe_stoull(result->get_value(0, "id")));
                return;
            }
            // INBOX 兜底（box_type=1，中文名邮箱可能直接查不到）
            std::string upper = name_utf8;
            std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
            if (upper != "INBOX") { cb(0); return; }
            (*conn)->async_query(db::sql::build_imap_get_inbox_id(),
                {std::to_string(user_id)},
                [conn, cb = std::move(cb)](std::shared_ptr<IDBResult> r2) mutable {
                    if (r2 && r2->get_row_count() > 0) cb(safe_stoull(r2->get_value(0, "id")));
                    else cb(0);
                });
        });
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::get_mailbox_mails_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t mailbox_id, uint64_t user_id,
    std::function<void(bool, std::vector<MailboxMailInfo>)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(false, {}); return; }
    (*conn)->async_query(db::sql::build_imap_get_mailbox_mails(),
        {std::to_string(mailbox_id), std::to_string(user_id)},
        [conn, cb = std::move(cb)](std::shared_ptr<IDBResult> result) mutable {
            std::vector<MailboxMailInfo> mails;
            if (!result) { cb(false, std::move(mails)); return; }
            for (size_t i = 0; i < result->get_row_count(); ++i) {
                MailboxMailInfo info;
                info.mail_id = safe_stoull(result->get_value(i, "id"));
                info.sender = result->get_value(i, "sender");
                info.recipient = result->get_value(i, "recipient");
                info.subject = result->get_value(i, "subject");
                info.body_path = result->get_value(i, "body_path");
                info.is_starred = result->get_value(i, "is_starred") == "1";
                info.is_deleted = result->get_value(i, "is_deleted") == "1";
                info.is_important = result->get_value(i, "is_important") == "1";
                info.status = result->get_value(i, "status").empty() ? 0 : static_cast<int>(safe_stoull(result->get_value(i, "status")));
                info.send_time = static_cast<time_t>(safe_stoull(result->get_value(i, "send_time")));
                mails.push_back(std::move(info));
            }
            cb(true, std::move(mails));
        });
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::get_mail_info_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t mail_id,
    std::function<void(bool, MailboxMailInfo)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(false, MailboxMailInfo{}); return; }
    (*conn)->async_query(
        "SELECT id, subject, body_path, UNIX_TIMESTAMP(send_time) AS send_time FROM mails WHERE id = ?",
        {std::to_string(mail_id)},
        [conn, cb = std::move(cb)](std::shared_ptr<IDBResult> result) mutable {
            MailboxMailInfo info;
            if (!result || result->get_row_count() == 0) { cb(false, info); return; }
            info.mail_id = safe_stoull(result->get_value(0, "id"));
            info.subject = result->get_value(0, "subject");
            info.body_path = result->get_value(0, "body_path");
            info.send_time = static_cast<time_t>(safe_stoull(result->get_value(0, "send_time")));
            cb(true, std::move(info));
        });
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::get_mail_sender_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t mail_id,
    std::function<void(std::string)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(""); return; }
    (*conn)->async_query(
        "SELECT sender FROM mail_recipients WHERE mail_id = ? LIMIT 1",
        {std::to_string(mail_id)},
        [conn, cb = std::move(cb)](std::shared_ptr<IDBResult> result) mutable {
            if (result && result->get_row_count() > 0) cb(result->get_value(0, "sender"));
            else cb("");
        });
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::get_mail_recipients_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t mail_id,
    std::function<void(std::vector<std::string>)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb({}); return; }
    (*conn)->async_query(
        "SELECT recipient FROM mail_recipients WHERE mail_id = ?",
        {std::to_string(mail_id)},
        [conn, cb = std::move(cb)](std::shared_ptr<IDBResult> result) mutable {
            std::vector<std::string> recipients;
            if (result) {
                for (size_t i = 0; i < result->get_row_count(); ++i) {
                    recipients.push_back(result->get_value(i, "recipient"));
                }
            }
            cb(std::move(recipients));
        });
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::get_user_email_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t user_id,
    std::function<void(std::string)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(""); return; }
    (*conn)->async_query(
        "SELECT mail_address FROM users WHERE id = ?",
        {std::to_string(user_id)},
        [conn, cb = std::move(cb)](std::shared_ptr<IDBResult> result) mutable {
            if (result && result->get_row_count() > 0) cb(result->get_value(0, "mail_address"));
            else cb("");
        });
}

// 邮件持久化（APPEND）。storage 写入是阻塞 I/O，调用方须在 worker 线程发起。
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::create_mail_async(
    const std::shared_ptr<storage::IStorageProvider>& storage,
    std::shared_ptr<ScopedConnection> conn,
    const std::string& subject, const std::string& body_content,
    std::function<void(uint64_t, std::string, std::string)> cb)
{
    if (!cb) return;
    if (!conn || !conn->is_valid()) { cb(0, "", "DB connection failed"); return; }
    int64_t mail_id = algorithm::get_snowflake_generator().next_id();

    std::string body_path;
    if (storage) {
        std::string storage_key = storage->build_mail_body_key(static_cast<uint64_t>(mail_id));
        storage::IoError err;
        if (!storage->append_binary(storage_key, body_content.data(),
                                    body_content.size(), err)) {
            cb(0, "", "Storage error: " + err.message);
            return;
        }
        body_path = storage_key;
    } else {
        std::string base = "mail/";
        std::string fp = base + std::to_string(mail_id);
        std::ofstream out(fp, std::ios::binary);
        if (!out) { cb(0, "", "Cannot write " + fp); return; }
        out.write(body_content.data(), static_cast<std::streamsize>(body_content.size()));
        if (!out) { cb(0, "", "Write failed " + fp); return; }
        body_path = fp;
    }

    auto ts = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    (*conn)->async_execute(db::sql::build_imap_append_mail_metadata(),
        {std::to_string(mail_id),
         db::sql::clip_subject_for_db(subject.empty() ? "(无主题)" : subject),
         body_path, std::to_string(ts)},
        // SQL 参数与回调捕获的求值顺序未指定，不能在同一调用中移动 body_path。
        [conn, storage, mail_id = static_cast<uint64_t>(mail_id), body_path,
         cb = std::move(cb)](bool ok) mutable {
            if (ok) { cb(mail_id, std::move(body_path), ""); return; }
            // mails 行写失败：体文件已先落，必须删掉，不留孤儿
            // （2026-10-21 生产实测失败路径残留 /opt/smtpServer/mail/<id>）。
            if (storage) {
                storage::IoError rm_err;
                if (!storage->remove_object(body_path, rm_err))
                    LOG_IMAP_ERROR("APPEND: failed to remove orphan body {}: {}",
                                   body_path, rm_err.message);
            } else {
                std::error_code rm_ec;
                std::filesystem::remove(body_path, rm_ec);
            }
            cb(0, "", "Insert mail record failed");
        });
}

// APPEND 半写回滚：三表显式删（不赌生产 FK 级联）+ 体文件删除。
// 任一 DB 删除失败都继续其余清理，最终以 removed=false 提示调用方留痕。
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::delete_mail_cascade_async(
    std::shared_ptr<ScopedConnection> conn,
    const std::shared_ptr<storage::IStorageProvider>& storage,
    uint64_t mail_id, std::function<void(bool)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(false); return; }
    auto step = [conn, storage, mail_id, cb = std::move(cb)](
                    bool ok1, bool ok2, bool ok3) mutable {
        bool removed = ok1 && ok2 && ok3;
        if (storage) {
            storage::IoError err;
            const std::string key = storage->build_mail_body_key(mail_id);
            if (storage->remove_object(key, err)) {
                // removed 语义只反映 DB 侧；文件删除失败仅记日志
            } else {
                LOG_IMAP_ERROR("APPEND rollback: failed to remove body {}: {}",
                               key, err.message);
            }
        }
        if (cb) cb(removed);
    };
    (*conn)->async_execute(db::sql::build_delete_mailbox_links_by_mail(),
        {std::to_string(mail_id)},
        [conn, mail_id, step = std::move(step)](bool ok1) mutable {
            (*conn)->async_execute(db::sql::build_delete_mail_recipients_by_mail(),
                {std::to_string(mail_id)},
                [conn, mail_id, step = std::move(step), ok1](bool ok2) mutable {
                    (*conn)->async_execute(db::sql::build_delete_mail_by_id(),
                        {std::to_string(mail_id)},
                        [step = std::move(step), ok1, ok2](bool ok3) mutable {
                            step(ok1, ok2, ok3);
                        });
                });
        });
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::link_mail_to_mailbox_async(
    std::shared_ptr<ScopedConnection> conn,
    uint64_t mail_id, uint64_t user_id, uint64_t mailbox_id,
    const std::string& sender, const std::string& recipient,
    int status, std::function<void(bool)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(false); return; }
    int64_t rid = algorithm::get_snowflake_generator().next_id();
    (*conn)->async_execute(db::sql::build_imap_append_mail_recipient(),
        {std::to_string(rid), std::to_string(mail_id), sender, recipient, std::to_string(status)},
        [conn, mail_id, mailbox_id, user_id, cb = std::move(cb)](bool ok) mutable {
            if (!ok) { cb(false); return; }
            // mail_mailbox.id 是 AUTO_INCREMENT，省略由 DB 生成；
            // UNIQUE KEY uk_mail_box_user 防重复关联。
            // SQL 走 builder：生产 mail_mailbox 无 add_time 列（2026-10-21 生产
            // 实测 Unknown column），与 SMTP 入站路径（persistent_queue）同款三列写法。
            (*conn)->async_execute(db::sql::build_imap_append_mailbox(),
                {std::to_string(mail_id), std::to_string(mailbox_id), std::to_string(user_id)},
                [conn, cb = std::move(cb)](bool ok2) mutable { cb(ok2); });
        });
}


// 缓存感知的邮箱统计（CPS）+ single-flight：同一 (user,mailbox) 只允许一个回源
// 查询链在途。缓存 miss/stale 时，首个连接成为 flight owner 发起查询，其余并发
// 连接挂到 owner 的等待列表上；owner 完成后写回缓存并统一通知所有等待者，任何
// 连接都不会挂死。防止 N 个客户端并发 SELECT 同一邮箱时各自查库打爆数据库。
//
// 锁序：flight 锁 → 缓存锁（仅 recheck 短暂取）。owner 完成路径是 cache_put
// （取放缓存锁）→ 再取 flight 锁移除条目，两锁从不嵌套持有，无锁序反转。
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::get_mailbox_stats_cached_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t user_id, uint64_t mailbox_id,
    std::function<void(MailboxCacheEntry, bool, bool)> cb)
{
    if (!cb) return;
    std::string key = mbox_cache_key(user_id, mailbox_id);

    // 1. 快路径：缓存命中且未过期，同步触发回调
    MailboxCacheEntry cached;
    bool had_entry = false, stale = false;
    if (m_mailboxStatsCache &&
        m_mailboxStatsCache->get(key, cached, stale)) {
        had_entry = true;
        if (!stale) {
            cb(cached, true, false);
            return;
        }
    }

    // 2. single-flight：拿 flight 锁。已有在途 → 挂等待回调，等 owner 完成统一通知；
    //    没有在途 → 复查缓存（owner 可能刚写完缓存还没来得及摘 flight；复查在
    //    flight 锁内做，消除"刚释放缓存锁数据才到"的窗口，也无需 cache+flight 二重锁）；
    //    仍 miss/stale → 创建 flight，自己同时是 owner 和第一个 waiter。
    std::shared_ptr<StatsFlight> flight;
    {
        std::lock_guard<std::mutex> lk(m_statsFlightsMutex);
        auto it = m_statsFlights.find(key);
        if (it != m_statsFlights.end()) {
            it->second->waiters.push_back(std::move(cb));
            return;
        }
        if (m_mailboxStatsCache) {
            MailboxCacheEntry recheck;
            bool recheck_stale = false;
            if (m_mailboxStatsCache->get(key, recheck, recheck_stale) && !recheck_stale) {
                cb(recheck, true, false);
                return;
            }
        }
        flight = std::make_shared<StatsFlight>();
        flight->waiters.push_back(std::move(cb));
        m_statsFlights[key] = flight;
    }

    // 3. owner：回源查询链 count → unseen → uidnext，完成后写缓存 + 通知所有等待者
    auto finish = [this, key, flight, had_entry, stale](MailboxCacheEntry entry) {
        if (m_mailboxStatsCache) {
            m_mailboxStatsCache->put(key, entry);
        }
        std::vector<std::function<void(MailboxCacheEntry, bool, bool)>> waiters;
        {
            std::lock_guard<std::mutex> lk(m_statsFlightsMutex);
            m_statsFlights.erase(key);
            if (flight) waiters = std::move(flight->waiters);
        }
        // 释放 flight 锁后才回调：等待者的 cb 可能重入本函数（drain 触发新 SELECT），
        // 持锁回调会非递归锁死锁。
        for (auto& w : waiters) {
            if (w) w(entry, had_entry, stale);
        }
    };

    get_mailbox_count_async(conn, mailbox_id, user_id,
        [conn, mailbox_id, user_id, finish = std::move(finish)](size_t exists) mutable {
            get_mailbox_unseen_count_async(conn, mailbox_id, user_id,
                [conn, mailbox_id, user_id, finish = std::move(finish), exists](size_t unseen) mutable {
                    get_mailbox_uidnext_async(conn, mailbox_id, user_id,
                        [mailbox_id, finish = std::move(finish), exists, unseen](uint64_t uidnext) mutable {
                            MailboxCacheEntry entry;
                            entry.exists = exists;
                            entry.unseen = unseen;
                            entry.uidnext = uidnext;
                            entry.uidvalidity = mailbox_id;
                            finish(entry);
                        });
                });
        });
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::get_mailbox_count_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t mailbox_id, uint64_t user_id,
    std::function<void(size_t)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(0); return; }
    (*conn)->async_query(db::sql::build_imap_select_status_total(),
        {std::to_string(mailbox_id), std::to_string(user_id)},
        [conn, cb = std::move(cb)](std::shared_ptr<IDBResult> result) mutable {
            if (result && result->get_row_count() > 0)
                cb(static_cast<size_t>(safe_stoull(result->get_value(0, "cnt"))));
            else cb(0);
        });
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::get_mailbox_unseen_count_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t mailbox_id, uint64_t user_id,
    std::function<void(size_t)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(0); return; }
    (*conn)->async_query(db::sql::build_imap_mailbox_unseen_count(),
        {std::to_string(user_id), std::to_string(mailbox_id), std::to_string(user_id)},
        [conn, cb = std::move(cb)](std::shared_ptr<IDBResult> result) mutable {
            if (result && result->get_row_count() > 0)
                cb(static_cast<size_t>(safe_stoull(result->get_value(0, "cnt"))));
            else cb(0);
        });
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::get_mailbox_uidnext_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t mailbox_id, uint64_t user_id,
    std::function<void(uint64_t)> cb)
{
    (void)user_id;   // mailbox_id 是 mailboxes 主键，全局唯一，无需 user_id 过滤
    if (!conn || !conn->is_valid()) { if (cb) cb(1); return; }
    // 原子推进高水位：行锁串行化并发/跨实例读者，GREATEST 防 expunge 后回退；
    // 推进后再读取（advance 是写，read 才是值）
    (*conn)->async_execute(db::sql::build_imap_uidnext_advance(),
        {std::to_string(mailbox_id), std::to_string(mailbox_id), std::to_string(mailbox_id)},
        [conn, mailbox_id, cb = std::move(cb)](bool) mutable {
            (*conn)->async_query(db::sql::build_imap_uidnext_read(),
                {std::to_string(mailbox_id)},
                // ⚠ 持 conn：外层回调返回后链上只剩此捕获撑住 ScopedConnection，
                // 漏捕 = advance 查询完成瞬间连接带飞归池（2026-09-24 crash 根因）
                [conn, cb = std::move(cb)](std::shared_ptr<IDBResult> result) mutable {
                    if (result && result->get_row_count() > 0)
                        cb(safe_stoull(result->get_value(0, "uidnext")));
                    else cb(1);
                });
        });
}

// ---------- 批量 flag 更新（STORE）：单条 IN SQL 拍平 N 循环 ----------

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::batch_mark_seen_async(
    std::shared_ptr<ScopedConnection> conn, const std::vector<uint64_t>& mail_ids,
    const std::string& recipient, int status, std::function<void(bool)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(false); return; }
    if (mail_ids.empty()) { if (cb) cb(true); return; }
    std::string sql = "UPDATE mail_recipients SET status = " + std::to_string(status)
        + " WHERE mail_id IN (" + join_mail_ids(mail_ids) + ")"
        + " AND recipient = '" + (*conn)->escape_string(recipient) + "'";
    (*conn)->async_execute(sql, [conn, cb = std::move(cb)](bool ok) mutable { if (cb) cb(ok); });
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::batch_mark_deleted_async(
    std::shared_ptr<ScopedConnection> conn, const std::vector<uint64_t>& mail_ids,
    uint64_t user_id, uint64_t mailbox_id, int deleted, std::function<void(bool)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(false); return; }
    if (mail_ids.empty()) { if (cb) cb(true); return; }
    std::string sql = "UPDATE mail_mailbox SET is_deleted = " + std::to_string(deleted)
        + " WHERE mail_id IN (" + join_mail_ids(mail_ids) + ")"
        + " AND user_id = " + std::to_string(user_id)
        + " AND mailbox_id = " + std::to_string(mailbox_id);
    (*conn)->async_execute(sql, [conn, cb = std::move(cb)](bool ok) mutable { if (cb) cb(ok); });
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::batch_mark_flagged_async(
    std::shared_ptr<ScopedConnection> conn, const std::vector<uint64_t>& mail_ids,
    uint64_t user_id, uint64_t mailbox_id, int flagged, std::function<void(bool)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(false); return; }
    if (mail_ids.empty()) { if (cb) cb(true); return; }
    std::string sql = "UPDATE mail_mailbox SET is_starred = " + std::to_string(flagged)
        + " WHERE mail_id IN (" + join_mail_ids(mail_ids) + ")"
        + " AND user_id = " + std::to_string(user_id)
        + " AND mailbox_id = " + std::to_string(mailbox_id);
    (*conn)->async_execute(sql, [conn, cb = std::move(cb)](bool ok) mutable { if (cb) cb(ok); });
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::expunge_mailbox_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t mailbox_id, uint64_t user_id,
    std::function<void(bool)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(false); return; }
    (*conn)->async_execute(db::sql::build_imap_expunge_delete_mailbox(),
        {std::to_string(mailbox_id), std::to_string(user_id)},
        [conn, cb = std::move(cb)](bool ok) mutable { if (cb) cb(ok); });
}

// ---------- COPY/MOVE 批量 ----------

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::batch_insert_mailbox_async(
    std::shared_ptr<ScopedConnection> conn, const std::vector<uint64_t>& mail_ids,
    uint64_t target_id, uint64_t user_id, std::function<void(size_t)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(0); return; }
    if (mail_ids.empty()) { if (cb) cb(0); return; }
    // INSERT IGNORE：uk_mail_box_user 自动去重（无需逐封查重），id 由
    // AUTO_INCREMENT 生成。实际插入数用 SELECT ROW_COUNT() 读（dup 被
    // IGNORE 的不计入）。
    // 列清单与生产 schema 对齐（无 add_time，2026-10-21 生产实测 Unknown column）；
    // is_starred/is_important/is_deleted 显式给 0 与旧行为一致。
    std::string values;
    bool first = true;
    for (uint64_t mid : mail_ids) {
        if (!first) values += ",";
        values += "(" + std::to_string(mid) + "," + std::to_string(target_id) + ","
               + std::to_string(user_id) + ",0,0,0)";
        first = false;
    }
    std::string sql = "INSERT IGNORE INTO mail_mailbox "
        "(mail_id, mailbox_id, user_id, is_starred, is_important, is_deleted) "
        "VALUES " + values;
    (*conn)->async_execute(sql,
        [conn, cb = std::move(cb)](bool ok) mutable {
            if (!ok) { if (cb) cb(0); return; }
            (*conn)->async_query(db::sql::build_select_row_count(),
                [conn, cb = std::move(cb)](std::shared_ptr<IDBResult> r) mutable {
                    size_t copied = (r && r->get_row_count() > 0)
                        ? static_cast<size_t>(safe_stoull(r->get_value(0, "affected"))) : 0;
                    cb(copied);
                });
        });
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::batch_mark_move_deleted_async(
    std::shared_ptr<ScopedConnection> conn, const std::vector<uint64_t>& mail_ids,
    uint64_t user_id, uint64_t source_mailbox_id, std::function<void(bool)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(false); return; }
    if (mail_ids.empty()) { if (cb) cb(true); return; }
    std::string sql = "UPDATE mail_mailbox SET is_deleted = 1"
        " WHERE mail_id IN (" + join_mail_ids(mail_ids) + ")"
        + " AND user_id = " + std::to_string(user_id)
        + " AND mailbox_id = " + std::to_string(source_mailbox_id);
    (*conn)->async_execute(sql, [conn, cb = std::move(cb)](bool ok) mutable { if (cb) cb(ok); });
}

} // namespace mail_system

#endif // TRADITIONAL_IMAPS_FSM_TPP
