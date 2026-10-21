#ifndef TRADITIONAL_IMAPS_FSM_TPP
#define TRADITIONAL_IMAPS_FSM_TPP

#include "mail_system/back/mailServer/imaps_server.h"
#include "mail_system/back/mailServer/fsm/imaps/imap_utils.h"
#include "mail_system/back/common/mime_parser.h"
#include "mail_system/back/common/safe_parse.h"
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
        auto config = std::atomic_load(&static_cast<MailServerBase*>(session->get_server())->m_config);
        username += "@" + config->system_domain;
        LOG_IMAP_DEBUG("Auto-domain applied: {}@{}", username, config->system_domain);
    }

    // bcrypt 是几十到几百毫秒的纯 CPU：绝不能在共享 io 线程上算 ——
    // 那会把同线程上所有其他会话的读写/握手/定时器全部卡住。
    // pause 后把「查 DB + 验密码」整体丢给 worker 线程池；回调在 worker
    // 线程触发，pause 期间独占 session（SPF/DNS/commit 回调同一约定）。
    // 登录者自己的延迟不变（bcrypt 该算多久还是多久），变的是别人不再陪等。
    auto authenticator = this->authenticator_;
    auto worker = this->m_workerThreadPool;
    if (!worker) {
        send_tagged(session, tag, "NO", "Server not ready");
        return;
    }
    session->set_paused(true);
    auto self = session->shared_from_this();
    worker->post(
        [self, tag, username = std::move(username), password = std::move(password),
         authenticator, ok_text, fail_text]() mutable {
            // 委托应用认证服务，完成后继续协议响应。
            // 底层 MySQL async_* 是默认同步包装（回调内联执行），链在 worker
            // 线程跑完；将来接真异步 DB，回调在 DB 线程触发，conn 由链内
            // shared ScopedConnection 保活，调用方结构无需改动。
            TraditionalImapsFsm<ConnectionType>::auth_user_async(
                authenticator, username, password,
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
            std::vector<MailboxSummary> mailboxes) mutable {
            if (!self || self->is_closed()) return;
            if (!ok) {
                send_tagged(self, tag, "OK", "LIST completed");
                self->drain_buffered_commands();
                return;
            }
            std::string response;
            for (const auto& mb : mailboxes) {
                const std::string& name = mb.name;
                int box_type = mb.box_type;

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
            std::vector<MailboxSummary> mailboxes) mutable {
            if (!self || self->is_closed()) return;
            std::string response;
            for (const auto& mb : mailboxes) {
                const std::string& name = mb.name;
                int box_type = mb.box_type;
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
    auto* srv = static_cast<MailServerBase*>(self->get_server());
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
    // flags 变化与删信都不推进 uidnext，列表及统计缓存靠写命令显式失效保一致；
    // TTL 只兜跨节点/跨协议改库（SMTP 投递、POP3 DELE）的底。
    if (m_mailboxListCache) m_mailboxListCache->invalidate(mbox_cache_key(user_id, mailbox_id));
    if (m_mailboxStatsCache) m_mailboxStatsCache->invalidate(mbox_cache_key(user_id, mailbox_id));
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
        auto metadata = mime_metadata_store_for(c.provider);
        const bool have_tree = need_parts && ensure_mime_tree(metadata, mail_info.body_path, body_content, mime_tree);
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
        } else if (auto metadata = mime_metadata_store_for(c.provider);
                   ensure_mime_tree(metadata, mail_info.body_path, body_content, c.struct_tree)) {
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

    // Metadata caching is an explicit provider capability; remote keys are never local paths.
    ctx->struct_sidecar_hit = false;
    if (ctx->want_body_struct) {
        auto metadata = mime_metadata_store_for(ctx->provider);
        ctx->struct_sidecar_hit = load_mime_tree(metadata, mail_info.body_path, ctx->struct_tree) &&
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
         mailbox_id, user_id, recipient_fallback = ctx->username](bool ok, std::vector<MailboxMailInfo> mails) mutable {
            if (!self || self->is_closed()) return;
            if (!ok || mails.empty()) {
                self->get_connection().executor()->post([self, tag = std::move(tag), ok] {
                    if (self->is_closed()) return;
                    send_tagged(self, tag, ok ? "OK" : "NO", ok ? "STORE completed" : "STORE failed: database error");
                    self->drain_buffered_commands();
                });
                return;
            }

            // Parse sequence ranges（支持逗号分隔）
            std::vector<std::pair<uint64_t, uint64_t>> ranges;
            expand_seq_set(seq_set, mails.size(), ranges);
            if (ranges.empty()) {
                self->get_connection().executor()->post([self, tag = std::move(tag)] {
                    if (self->is_closed()) return;
                    send_tagged(self, tag, "OK", "STORE completed");
                    self->drain_buffered_commands();
                });
                return;
            }

            repository::ImapStoreRequest request;
            request.scope = {user_id, mailbox_id};
            request.recipient_fallback = std::move(recipient_fallback);
            if (flag_seen) request.seen = add;
            if (flag_flagged) request.flagged = add;
            if (flag_deleted) request.deleted = add;
            std::string response;
            for (const auto& range : ranges) {
                for (uint64_t seq = range.first; seq <= range.second; ++seq) {
                    const auto& mail_info = mails[seq - 1];
                    request.mail_ids.push_back(mail_info.mail_id);
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
            }
            m_mutationRepository->store_async(conn, std::move(request),
                [this, self, tag = std::move(tag), response = std::move(response),
                 mailbox_id, user_id](repository::ImapMutationResult result) mutable {
                    if (self->is_closed()) return;
                    self->get_connection().executor()->post(
                        [this, self, tag = std::move(tag), response = std::move(response),
                         mailbox_id, user_id, result]() mutable {
                            if (self->is_closed()) return;
                            invalidate_mailbox_list(user_id, mailbox_id);
                            if (!result.ok) {
                                send_tagged(self, tag, "NO", "STORE failed: database error");
                                self->drain_buffered_commands();
                                return;
                            }
                            response += tag + " OK STORE completed\r\n";
                            self->do_async_write(response, [](auto s, auto& ec) { if (!ec) s->do_async_read(); });
                            self->drain_buffered_commands();
                        });
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
                self->get_connection().executor()->post([this, self, tag = std::move(tag)]() mutable {
                    if (self->is_closed()) return;
                    send_tagged(self, tag, "NO", "Server error");
                    self->drain_buffered_commands();
                });
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
            m_mutationRepository->expunge_async(
                conn, {user_id, mailbox_id},
                [this, self, tag = std::move(tag), expunged_seqs = std::move(expunged_seqs),
                 mailbox_id, user_id](repository::ImapMutationResult result) mutable {
                    if (self->is_closed()) return;
                    self->get_connection().executor()->post(
                        [this, self, tag = std::move(tag), expunged_seqs = std::move(expunged_seqs),
                         mailbox_id, user_id, result]() mutable {
                            if (self->is_closed()) return;
                            invalidate_mailbox_list(user_id, mailbox_id);
                            if (!result.ok) {
                                send_tagged(self, tag, "NO", "EXPUNGE failed: database error");
                                self->drain_buffered_commands();
                                return;
                            }
                            std::string response;
                            for (auto seq : expunged_seqs)
                                response += "* " + std::to_string(seq) + " EXPUNGE\r\n";
                            response += tag + " OK EXPUNGE completed\r\n";
                            self->do_async_write(response, [](auto s, auto& ec) { if (!ec) s->do_async_read(); });
                            self->drain_buffered_commands();
                        });
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
            m_mutationRepository->expunge_async(
                conn, {user_id, mailbox_id},
                [this, self, finish, tag, mailbox_id, user_id](repository::ImapMutationResult result) {
                    if (self->is_closed()) return;
                    self->get_connection().executor()->post([this, self, finish, tag, mailbox_id, user_id, result] {
                        if (self->is_closed()) return;
                        invalidate_mailbox_list(user_id, mailbox_id);
                        if (!result.ok) {
                            send_tagged(self, tag, "NO", "CLOSE failed: database error");
                            self->drain_buffered_commands();
                            return;
                        }
                        finish([self]() { self->drain_buffered_commands(); });
                    });
                });
            return;
        }
        send_tagged(session, tag, "NO", "CLOSE failed: database unavailable");
        return;
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
            auto server = static_cast<ImapsServer*>(static_cast<MailServerBase*>(self->get_server()));
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
    m_folderRepository->create_async(conn, ctx->user_id, std::move(mailbox_name),
        [session, tag](repository::ImapFolderResult result) {
            if (session->is_closed()) return;
            session->get_connection().executor()->post([session, tag, result] {
                if (session->is_closed()) return;
                if (result.status == repository::ImapFolderStatus::ok) send_tagged(session, tag, "OK", "CREATE completed");
                else send_tagged(session, tag, "NO", "CREATE failed (maybe already exists)");
                session->drain_buffered_commands();
            });
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
                self->get_connection().executor()->post([self, tag = std::move(tag)] {
                    if (self->is_closed()) return;
                    send_tagged(self, tag, "NO", "Mailbox not found");
                    self->drain_buffered_commands();
                });
                return;
            }

            m_folderRepository->remove_async(conn, {user_id, mailbox_id},
                [this, self, tag = std::move(tag), mailbox_id, user_id](repository::ImapFolderResult result) mutable {
                    if (self->is_closed()) return;
                    self->get_connection().executor()->post([this, self, tag = std::move(tag), mailbox_id, user_id, result] {
                        if (self->is_closed()) return;
                        if (result.may_have_changed) invalidate_mailbox_list(user_id, mailbox_id);
                        using Status = repository::ImapFolderStatus;
                        if (result.status == Status::ok) send_tagged(self, tag, "OK", "DELETE completed");
                        else if (result.status == Status::protected_mailbox) send_tagged(self, tag, "NO", "Cannot delete system mailbox");
                        else if (result.status == Status::not_found) send_tagged(self, tag, "NO", "Mailbox not found");
                        else send_tagged(self, tag, "NO", "DELETE failed");
                        self->drain_buffered_commands();
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
        [this, self, conn, tag = std::move(tag), user_id, new_name = std::move(new_name)](
            uint64_t mailbox_id) mutable {
            if (!self || self->is_closed()) return;
            if (mailbox_id == 0) {
                self->get_connection().executor()->post([self, tag = std::move(tag)] {
                    if (self->is_closed()) return;
                    send_tagged(self, tag, "NO", "Mailbox not found");
                    self->drain_buffered_commands();
                });
                return;
            }
            m_folderRepository->rename_async(conn, {user_id, mailbox_id}, std::move(new_name),
                [this, self, tag = std::move(tag), user_id, mailbox_id](repository::ImapFolderResult result) mutable {
                    if (self->is_closed()) return;
                    self->get_connection().executor()->post([this, self, tag = std::move(tag), user_id, mailbox_id, result] {
                        if (self->is_closed()) return;
                        if (result.may_have_changed) invalidate_mailbox_list(user_id, mailbox_id);
                        if (result.status == repository::ImapFolderStatus::ok) send_tagged(self, tag, "OK", "RENAME completed");
                        else send_tagged(self, tag, "NO", "RENAME failed");
                        self->drain_buffered_commands();
                    });
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

    auto conn = this->acquire_connection(ctx->shard_index);
    if (!conn->is_valid()) {
        send_tagged(session, tag, "NO", "Server database unavailable");
        return;
    }
    session->set_paused(true);
    auto self = session->shared_from_this();
    ImapAppendRequest request;
    request.user_id = ctx->user_id;
    request.subject = std::move(subject);
    request.body = std::move(body_content);
    request.initial_status = init_status;
    request.authenticated_address = ctx->username;
    const auto user_id = request.user_id;
    find_mailbox_id_async(conn, user_id, mailbox_name,
        [this, self, conn, tag = std::move(tag), request = std::move(request)](uint64_t mailbox_id) mutable {
            if (self->is_closed()) return;
            if (!mailbox_id) {
                self->get_connection().executor()->post([self, tag = std::move(tag)] {
                    if (self->is_closed()) return;
                    send_tagged(self, tag, "NO", "APPEND failed: mailbox not found");
                    self->drain_buffered_commands();
                });
                return;
            }
            const auto user = request.user_id;
            request.mailbox_id = mailbox_id;
            m_appendService->append_async(conn, get_storage(0), std::move(request),
                [this, self, tag = std::move(tag), user, mailbox_id](ImapAppendResult result) mutable {
                    if (self->is_closed()) return;
                    self->get_connection().executor()->post(
                        [this, self, tag = std::move(tag), user, mailbox_id, result = std::move(result)]() mutable {
                            if (self->is_closed()) return;
                            invalidate_mailbox_list(user, mailbox_id);
                            if (!result.ok) {
                                if (!result.cleanup_complete) LOG_IMAP_ERROR("APPEND: rollback incomplete, orphan may remain");
                                if (result.error == "Server not ready") send_tagged(self, tag, "NO", result.error);
                                else send_tagged(self, tag, "NO", "[SERVERBUG] APPEND failed" +
                                    (result.error.empty() ? std::string{} : ": " + result.error));
                            } else {
                                const std::string response = tag + " OK [APPENDUID " + std::to_string(mailbox_id)
                                    + " " + std::to_string(result.mail_id) + "] APPEND completed\r\n";
                                self->do_async_write(response, [](auto s, auto& ec) { if (!ec) s->do_async_read(); });
                            }
                            self->drain_buffered_commands();
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
        // A provider failure is not permission to interpret its opaque key as a local path.
        LOG_FILE_IO_ERROR("Failed to read SEARCH body {}: {}", body_path, err.message);
        return {};
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

                    repository::ImapCopyMoveRequest request;
                    request.source = {user_id, source_mailbox_id};
                    request.target_mailbox_id = target_id;
                    request.mail_ids = std::move(mail_ids);
                    request.move = is_move;
                    m_mutationRepository->copy_move_async(conn, std::move(request),
                        [this, self, tag = std::move(tag), is_move,
                         user_id, source_mailbox_id, target_id](repository::ImapCopyMoveResult result) mutable {
                            if (self->is_closed()) return;
                            self->get_connection().executor()->post(
                                [this, self, tag = std::move(tag), is_move, user_id,
                                 source_mailbox_id, target_id, result]() mutable {
                                    if (self->is_closed()) return;
                                    // Even a failed chain may have copied target rows
                                    // or partially marked the source. Invalidate both.
                                    invalidate_mailbox_list(user_id, target_id);
                                    if (is_move) invalidate_mailbox_list(user_id, source_mailbox_id);
                                    const std::string command = is_move ? "MOVE" : "COPY";
                                    if (!result.copy_ok || (is_move && !result.source_marked)) {
                                        send_tagged(self, tag, "NO", command + " failed: database error");
                                        self->drain_buffered_commands();
                                        return;
                                    }
                                    std::string response = tag + " OK " + command
                                        + " completed (" + std::to_string(result.copied) + " messages)\r\n";
                                    self->do_async_write(response, [](auto s, auto& ec) { if (!ec) s->do_async_read(); });
                                    self->drain_buffered_commands();
                                });
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
// 应用仓储 facade：协议不依赖 SQL、结果集列名或具体数据库驱动。
// 仓储保留连接租约，协议续作维持会话 executor 约束。
// ====================================================================
// 用户认证（仿 POP3 auth_user_async / SMTP auth 链）。调用方须在 worker
// 线程发起（bcrypt 几十~几百 ms 纯 CPU，不许在 io 线程算）。
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::auth_user_async(
    const std::shared_ptr<IAccountAuthenticator>& authenticator,
    const std::string& mail_address, const std::string& password,
    std::function<void(bool, uint64_t, int)> complete)
{
    if (!complete) return;
    if (!authenticator) { complete(false, 0, 0); return; }
    authenticator->login_async(mail_address, password,
        [complete = std::move(complete)](AccountAuthResult result) {
            complete(result.ok, result.user_id, result.shard);
        });
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::get_mailboxes_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t user_id,
    std::function<void(bool, std::vector<MailboxSummary>)> cb)
{
    m_mailboxRepository->get_mailboxes_async(std::move(conn), user_id, std::move(cb));
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::find_mailbox_id_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t user_id,
    const std::string& mailbox_name, std::function<void(uint64_t)> cb)
{
    std::string name_utf8 = mailbox_name;
    if (mailbox_name.find('&') != std::string::npos) {
        auto decoded = decode_imap_utf7(mailbox_name);
        if (!decoded.empty()) name_utf8 = std::move(decoded);
    }
    m_mailboxRepository->find_mailbox_id_async(std::move(conn), user_id, name_utf8, std::move(cb));
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::get_mailbox_mails_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t mailbox_id, uint64_t user_id,
    std::function<void(bool, std::vector<MailboxMailInfo>)> cb)
{
    m_mailboxRepository->get_mailbox_mails_async(std::move(conn), mailbox_id, user_id, std::move(cb));
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::get_mail_info_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t id,
    std::function<void(bool, MailboxMailInfo)> complete)
{
    m_messageRepository->get_mail_async(std::move(conn), id, std::move(complete));
}
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::get_mail_sender_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t id,
    std::function<void(std::string)> complete)
{
    m_messageRepository->get_sender_async(std::move(conn), id, std::move(complete));
}
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::get_mail_recipients_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t id,
    std::function<void(std::vector<std::string>)> complete)
{
    m_messageRepository->get_recipients_async(std::move(conn), id, std::move(complete));
}
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::get_user_email_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t id,
    std::function<void(std::string)> complete)
{
    m_messageRepository->get_user_address_async(std::move(conn), id, std::move(complete));
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

    auto repository = m_mailboxRepository;
    repository->get_mailbox_count_async(conn, mailbox_id, user_id,
        [repository, conn, mailbox_id, user_id, finish = std::move(finish)](size_t exists) mutable {
            repository->get_mailbox_unseen_count_async(conn, mailbox_id, user_id,
                [repository, conn, mailbox_id, user_id, finish = std::move(finish), exists](size_t unseen) mutable {
                    repository->get_mailbox_uidnext_async(conn, mailbox_id, user_id,
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
    m_mailboxRepository->get_mailbox_count_async(std::move(conn), mailbox_id, user_id, std::move(cb));
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::get_mailbox_unseen_count_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t mailbox_id, uint64_t user_id,
    std::function<void(size_t)> cb)
{
    m_mailboxRepository->get_mailbox_unseen_count_async(std::move(conn), mailbox_id, user_id, std::move(cb));
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::get_mailbox_uidnext_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t mailbox_id, uint64_t user_id,
    std::function<void(uint64_t)> cb)
{
    m_mailboxRepository->get_mailbox_uidnext_async(std::move(conn), mailbox_id, user_id, std::move(cb));
}

} // namespace mail_system

#endif // TRADITIONAL_IMAPS_FSM_TPP
