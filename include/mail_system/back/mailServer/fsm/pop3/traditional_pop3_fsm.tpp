#ifndef TRADITIONAL_POP3_FSM_TPP
#define TRADITIONAL_POP3_FSM_TPP

#include "mail_system/back/mailServer/fsm/pop3/traditional_pop3_fsm.h"
#include "mail_system/back/mailServer/mail_server_base.h"
#include "mail_system/back/common/logger.h"
#include "mail_system/back/storage/i_storage_provider.h"  // POP3 正文读取走 provider
#include "mail_system/back/algorithm/snow.h"
#include "framework/connection/tcp_connection.h"
#include "framework/connection/ssl_connection.h"
#include "framework/db/db_pool.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <cstdint>
#include <exception>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <sstream>
#include <utility>

namespace mail_system {

template <typename ConnectionType>
Pop3Context* TraditionalPop3Fsm<ConnectionType>::ctx_of(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session) {
    return static_cast<Pop3Context*>(session->get_context());
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::send_line(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session, const std::string& line) {
    if (!session) return;
    std::string out = line;
    if (out.empty() || (out.back() != '\n')) out += "\r\n";
    session->trace_append_outbound(out);
    session->do_async_write(out);
}

template <typename ConnectionType>
std::string TraditionalPop3Fsm<ConnectionType>::get_state_name(Pop3State s) {
    switch (s) {
        case Pop3State::INIT: return "INIT";
        case Pop3State::AUTHORIZATION: return "AUTHORIZATION";
        case Pop3State::TRANSACTION: return "TRANSACTION";
        case Pop3State::UPDATE: return "UPDATE";
        case Pop3State::CLOSED: return "CLOSED";
        default: return "UNKNOWN_STATE";
    }
}

template <typename ConnectionType>
std::string TraditionalPop3Fsm<ConnectionType>::get_event_name(Pop3Event e) {
    switch (e) {
        case Pop3Event::CONNECT: return "CONNECT";
        case Pop3Event::CAPA: return "CAPA";
        case Pop3Event::USER: return "USER";
        case Pop3Event::PASS: return "PASS";
        case Pop3Event::STAT: return "STAT";
        case Pop3Event::LIST: return "LIST";
        case Pop3Event::UIDL: return "UIDL";
        case Pop3Event::RETR: return "RETR";
        case Pop3Event::DELE: return "DELE";
        case Pop3Event::NOOP: return "NOOP";
        case Pop3Event::RSET: return "RSET";
        case Pop3Event::QUIT: return "QUIT";
        case Pop3Event::ERROR: return "ERROR";
        case Pop3Event::TIMEOUT: return "TIMEOUT";
        default: return "UNKNOWN_EVENT";
    }
}

// ====================================================================
// Authentication and compatibility repository facades
// ====================================================================

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::auth_user_async(
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
void TraditionalPop3Fsm<ConnectionType>::get_inbox_id_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t user_id, std::function<void(uint64_t)> cb)
{
    repository::make_sql_pop3_mailbox_repository()->get_inbox_async(repository::borrow_pop3_db_context(std::move(conn)), user_id,
        [cb = std::move(cb)](bool ok, uint64_t id) { if (cb) cb(ok ? id : 0); });
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::get_inbox_mails_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t mailbox_id, uint64_t user_id,
    std::function<void(std::vector<Pop3Message>)> cb)
{
    repository::make_sql_pop3_mailbox_repository()->get_messages_async(repository::borrow_pop3_db_context(std::move(conn)), user_id, mailbox_id,
        [cb = std::move(cb)](bool, std::vector<MailboxMailInfo> rows) {
            std::vector<Pop3Message> messages;
            for (auto& row : rows) if (row.mail_id) messages.push_back({row.mail_id, std::move(row.body_path), 0});
            if (cb) cb(std::move(messages));
        });
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::acquire_lock_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t user_id, const std::string& sid, std::function<void(bool)> cb)
{
    repository::make_sql_pop3_lock_repository()->acquire_async(repository::borrow_pop3_db_context(std::move(conn)), {user_id, sid},
        [cb = std::move(cb)](repository::Pop3LockStatus status) { if (cb) cb(status == repository::Pop3LockStatus::owned); });
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::release_lock_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t user_id, const std::string& sid, std::function<void()> cb)
{
    repository::make_sql_pop3_lock_repository()->release_async(repository::borrow_pop3_db_context(std::move(conn)), {user_id, sid},
        [cb = std::move(cb)](bool) { if (cb) cb(); });
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::renew_lock_heartbeat_async(
    const std::shared_ptr<router::IShardRouter>& router, uint64_t user_id,
    const std::string& sid, int shard, std::function<void(bool)> cb)
{
    auto context = repository::make_pop3_db_context(router ? router->get_db_pool(static_cast<size_t>(shard)) : nullptr);
    repository::make_sql_pop3_lock_repository()->renew_async(std::move(context), {user_id, sid},
        [cb = std::move(cb)](repository::Pop3LockStatus status) { if (cb) cb(status == repository::Pop3LockStatus::owned); });
}

template <typename ConnectionType>
bool TraditionalPop3Fsm<ConnectionType>::sweep_expired_locks(const std::shared_ptr<router::IShardRouter>& router)
{
    if (!router) return false;
    auto locks = repository::make_sql_pop3_lock_repository();
    bool ok = true;
    for (size_t shard = 0; shard < router->shard_count(); ++shard) {
        auto pool = router->get_db_pool(shard);
        if (!pool) continue;
        if (!locks->sweep_expired(repository::make_pop3_db_context(std::move(pool)))) ok = false;
    }
    return ok;
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::start_heartbeat(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session,
    std::shared_ptr<router::IShardRouter> router, std::shared_ptr<pr::ThreadPoolBase> worker,
    std::chrono::milliseconds interval, std::shared_ptr<repository::IPop3LockRepository> locks)
{
    if (!session || !router || !worker || !locks) return;
    session->get_connection().executor()->post([session, router, worker, interval, locks] {
        if (session->is_closed()) return;
        auto* ctx = ctx_of(session);
        if (!ctx || !ctx->is_authenticated || ctx->dropped || ctx->session_id.empty() || ctx->heartbeat_timer) return;
        auto timer = session->get_connection().executor()->make_timer();
        ctx->heartbeat_timer = timer;
        const auto user_id = ctx->user_id;
        const auto sid = ctx->session_id;
        const auto shard = ctx->shard_index;
        auto weak_self = std::weak_ptr<pr::SessionBase<ConnectionType>>(session);
        auto weak_timer = std::weak_ptr<pr::net::ITimer>(timer);
        auto tick = std::make_shared<pr::net::ITimer::Handler>();
        ctx->heartbeat_handler = tick;
        auto weak_tick = std::weak_ptr<pr::net::ITimer::Handler>(tick);
        *tick = [weak_self, weak_timer, weak_tick, router, worker, locks,
                 user_id, sid, shard, interval](const std::error_code& error) {
            auto self = weak_self.lock();
            if (!self || self->is_closed()) return;
            auto* ctx = ctx_of(self);
            if (!ctx || ctx->dropped || ctx->heartbeat_timer != weak_timer.lock()) return;
            if (error) {
                if (error != std::errc::operation_canceled)
                    LOG_SESSION_ERROR("POP3 heartbeat timer failed for user={}: {}", user_id, error.message());
                return;
            }
            auto completed = std::make_shared<std::atomic<bool>>(false);
            auto complete = [weak_self, weak_timer, weak_tick, user_id, interval, completed](repository::Pop3LockStatus status) {
                if (completed->exchange(true)) return;
                auto self = weak_self.lock();
                if (!self || self->is_closed()) return;
                self->get_connection().executor()->post([weak_self, weak_timer, weak_tick, user_id, interval, status] {
                    auto self = weak_self.lock();
                    if (!self || self->is_closed()) return;
                    auto timer = weak_timer.lock();
                    auto next = weak_tick.lock();
                    auto* ctx = ctx_of(self);
                    if (!ctx || ctx->dropped || !timer || !next || ctx->heartbeat_timer != timer) return;
                    if (status != repository::Pop3LockStatus::owned) {
                        LOG_SESSION_WARN("POP3 heartbeat could not confirm lock for user={}, closing session", user_id);
                        self->close();
                        return;
                    }
                    timer->expires_after(interval);
                    timer->async_wait(*next);
                });
            };
            try {
                worker->post([weak_self, router, locks, user_id, sid, shard, complete] {
                    auto self = weak_self.lock();
                    if (!self || self->is_closed()) return;
                    try {
                        auto context = repository::make_pop3_db_context(router->get_db_pool(static_cast<size_t>(shard)));
                        locks->renew_async(context, {user_id, sid}, complete);
                    } catch (...) { complete(repository::Pop3LockStatus::database_error); }
                });
            } catch (...) { complete(repository::Pop3LockStatus::database_error); }
        };
        timer->expires_after(interval);
        timer->async_wait(*tick);
        LOG_SESSION_INFO("POP3 heartbeat started for user={} ({}ms)", user_id, interval.count());
    });
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::apply_deletions_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t user_id, uint64_t mailbox_id,
    const std::set<uint64_t>& ids, std::function<void(bool)> cb)
{
    repository::make_sql_pop3_mailbox_repository()->apply_deletions_async(repository::borrow_pop3_db_context(std::move(conn)),
        {user_id, mailbox_id, ids}, [cb = std::move(cb)](repository::Pop3DeletionResult result) { if (cb) cb(result.ok); });
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::init_transition_table() {
    this->add_transition(Pop3State::INIT, Pop3Event::CONNECT, Pop3State::AUTHORIZATION);
    this->add_transition(Pop3State::AUTHORIZATION, Pop3Event::USER, Pop3State::AUTHORIZATION);
    this->add_transition(Pop3State::AUTHORIZATION, Pop3Event::PASS, Pop3State::TRANSACTION);
    this->add_transition(Pop3State::AUTHORIZATION, Pop3Event::CAPA, Pop3State::AUTHORIZATION);
    this->add_transition(Pop3State::AUTHORIZATION, Pop3Event::NOOP, Pop3State::AUTHORIZATION);
    this->add_transition(Pop3State::AUTHORIZATION, Pop3Event::QUIT, Pop3State::UPDATE);
    this->add_transition(Pop3State::TRANSACTION, Pop3Event::STAT, Pop3State::TRANSACTION);
    this->add_transition(Pop3State::TRANSACTION, Pop3Event::LIST, Pop3State::TRANSACTION);
    this->add_transition(Pop3State::TRANSACTION, Pop3Event::UIDL, Pop3State::TRANSACTION);
    this->add_transition(Pop3State::TRANSACTION, Pop3Event::RETR, Pop3State::TRANSACTION);
    this->add_transition(Pop3State::TRANSACTION, Pop3Event::DELE, Pop3State::TRANSACTION);
    this->add_transition(Pop3State::TRANSACTION, Pop3Event::NOOP, Pop3State::TRANSACTION);
    this->add_transition(Pop3State::TRANSACTION, Pop3Event::RSET, Pop3State::TRANSACTION);
    this->add_transition(Pop3State::TRANSACTION, Pop3Event::CAPA, Pop3State::TRANSACTION);
    this->add_transition(Pop3State::TRANSACTION, Pop3Event::QUIT, Pop3State::UPDATE);
    // ERROR = 未知命令/空行：回 -ERR 并留在当前状态（不自闭）
    for (int i = 0; i <= static_cast<int>(Pop3State::UPDATE); ++i) {
        auto s = static_cast<Pop3State>(i);
        this->add_transition(s, Pop3Event::ERROR, s);
    }
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::init_state_handlers() {
    this->add_handler(Pop3State::INIT, Pop3Event::CONNECT,
        [this](auto session) { handle_init_connect(session); });
    this->add_handler(Pop3State::AUTHORIZATION, Pop3Event::USER,
        [this](auto session) { handle_user(session); });
    this->add_handler(Pop3State::AUTHORIZATION, Pop3Event::PASS,
        [this](auto session) { handle_pass(session); });
    this->add_handler(Pop3State::AUTHORIZATION, Pop3Event::CAPA,
        [this](auto session) { handle_capa(session); });
    this->add_handler(Pop3State::AUTHORIZATION, Pop3Event::NOOP,
        [this](auto session) { handle_noop(session); });
    this->add_handler(Pop3State::AUTHORIZATION, Pop3Event::QUIT,
        [this](auto session) { handle_quit(session); });
    this->add_handler(Pop3State::TRANSACTION, Pop3Event::STAT,
        [this](auto session) { handle_stat(session); });
    this->add_handler(Pop3State::TRANSACTION, Pop3Event::LIST,
        [this](auto session) { handle_list(session); });
    this->add_handler(Pop3State::TRANSACTION, Pop3Event::UIDL,
        [this](auto session) { handle_uidl(session); });
    this->add_handler(Pop3State::TRANSACTION, Pop3Event::RETR,
        [this](auto session) { handle_retr(session); });
    this->add_handler(Pop3State::TRANSACTION, Pop3Event::DELE,
        [this](auto session) { handle_dele(session); });
    this->add_handler(Pop3State::TRANSACTION, Pop3Event::NOOP,
        [this](auto session) { handle_noop(session); });
    this->add_handler(Pop3State::TRANSACTION, Pop3Event::RSET,
        [this](auto session) { handle_rset(session); });
    this->add_handler(Pop3State::TRANSACTION, Pop3Event::CAPA,
        [this](auto session) { handle_capa(session); });
    this->add_handler(Pop3State::TRANSACTION, Pop3Event::QUIT,
        [this](auto session) { handle_quit(session); });
    for (int i = 0; i <= static_cast<int>(Pop3State::UPDATE); ++i) {
        auto s = static_cast<Pop3State>(i);
        this->add_handler(s, Pop3Event::ERROR,
            [this](auto session) { handle_error(session); });
    }
}

template <typename ConnectionType>
bool TraditionalPop3Fsm<ConnectionType>::is_terminal_state(Pop3State s) const {
    return s == Pop3State::CLOSED;
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::on_invalid_transition(
    Pop3State, Pop3Event, std::shared_ptr<pr::SessionBase<ConnectionType>> session) {
    if (!session || session->is_closed()) return;
    send_line(session, "-ERR Command not allowed in current state");
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::on_handler_not_found(
    Pop3State, Pop3Event, std::shared_ptr<pr::SessionBase<ConnectionType>> session) {
    if (!session || session->is_closed()) return;
    send_line(session, "-ERR Internal: handler missing");
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::process_event(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session, Pop3Event event)
{
    if (!session) return;
    Pop3State cur = static_cast<Pop3State>(session->get_current_state());
    this->dispatch(session, cur, event);
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::auto_process_event(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    if (!session) return;
    process_event(session, static_cast<Pop3Event>(session->get_next_event()));
}

// ====================================================================
// 状态处理器
// ====================================================================

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::handle_init_connect(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session) {
    if (!session) return;
    long now = static_cast<long>(time(nullptr));
    char ts[32];
    snprintf(ts, sizeof(ts), "%ld", now);
    send_line(session, std::string("+OK ProtoRelay POP3 server ready <") + ts + "@pop3>");
    session->set_current_state(static_cast<int>(Pop3State::AUTHORIZATION));
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::handle_capa(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session) {
    if (!session) return;
    send_line(session, "+OK Capability list follows\r\nUSER\r\nUIDL\r\n"
                       "IMPLEMENTATION ProtoRelay-POP3\r\n.\r\n");
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::handle_user(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session) {
    if (!session) return;
    auto* ctx = ctx_of(session);
    if (!ctx) { send_line(session, "-ERR Internal: no context"); return; }
    std::string args = session->get_last_command_args();
    if (args.empty()) { send_line(session, "-ERR USER requires a name argument"); return; }
    ctx->clear();
    ctx->username = args;
    send_line(session, "+OK Send PASS");
}

// Holds only adapter state: scheduling, session context, protocol replies, and
// cleanup policy. Repositories alone interpret SQL and database result columns.
template <typename ConnectionType>
struct TraditionalPop3Fsm<ConnectionType>::LoginOperation
    : std::enable_shared_from_this<LoginOperation> {
    std::shared_ptr<pr::SessionBase<ConnectionType>> session;
    std::shared_ptr<router::IShardRouter> router;
    std::shared_ptr<pr::ThreadPoolBase> worker;
    std::shared_ptr<IAccountAuthenticator> authenticator;
    std::shared_ptr<repository::IPop3MailboxRepository> mailboxes;
    std::shared_ptr<repository::IPop3LockRepository> locks;
    repository::Pop3DbContext database;
    std::chrono::milliseconds interval;
    std::string email, password, sid;
    uint64_t user = 0, inbox = 0;
    int shard = 0;
    bool lock_attempted = false;
    std::atomic<bool> stopping{false}, replied{false};

    template <typename Task> void guard(Task task) {
        if (stopping.load()) return;
        try { task(); }
        catch (const std::exception& error) {
            LOG_SESSION_ERROR("POP3 login continuation failed: {}", error.what());
            fail("-ERR Server database unavailable", "server_error");
        } catch (...) {
            LOG_SESSION_ERROR("POP3 login continuation failed with unknown exception");
            fail("-ERR Server database unavailable", "server_error");
        }
    }
    template <typename Task> void queue(Task task) {
        auto self = this->shared_from_this();
        try {
            if (!worker) { fail("-ERR Server not ready", "no_worker"); return; }
            worker->post([self, task = std::move(task)]() mutable { self->guard(std::move(task)); });
        } catch (...) { fail("-ERR Server not ready", "no_worker"); }
    }
    void reply_failure(std::string reply, std::string label, bool auth_failure) {
        if (replied.exchange(true)) return;
        auto self = this->shared_from_this();
        session->get_connection().executor()->post([self, reply = std::move(reply), label = std::move(label), auth_failure]() mutable {
            if (self->session->is_closed() || reply.empty()) return;
            auto session = self->session;
            session->set_paused(false);
            auto* srv = static_cast<MailServerBase*>(session->get_server());
            const bool close = auth_failure && session->record_auth_failure_and_check();
            if (close) { reply = "-ERR Too many auth failures, closing connection"; label = "fail_too_many"; }
            send_line(session, reply);
            if (srv) if (auto metrics = srv->get_metrics().lock())
                metrics->inc_counter("protorelay_pop3_auth_total", {{"result", label}}, 1);
            if (close) session->close_after_flush();
            else session->drain_buffered_commands();
        });
    }
    void fail(std::string reply, std::string label, bool auth_failure = false) {
        if (stopping.exchange(true)) return;
        if (!lock_attempted || !database) {
            reply_failure(std::move(reply), std::move(label), auth_failure); return;
        }
        // This path also runs after peer disconnect and uncertain acquisition.
        // Cleanup must outlive protocol liveness and may itself fail or throw.
        auto self = this->shared_from_this();
        auto release = [self, reply = std::move(reply), label = std::move(label), auth_failure]() mutable {
            auto complete = [self, reply, label, auth_failure](bool released) {
                if (!released) LOG_SESSION_WARN("POP3 login lock cleanup failed; expiry sweep will retry");
                self->reply_failure(reply, label, auth_failure);
            };
            try { self->locks->release_async(self->database, {self->user, self->sid}, complete); }
            catch (...) { complete(false); }
        };
        try { worker->post(release); }
        catch (...) { reply_failure("-ERR Server not ready", "no_worker", false); }
    }
    void start() {
        auto self = this->shared_from_this();
        queue([self] {
            if (!self->authenticator) { self->fail("-ERR Authentication failed", "wrong_pass", true); return; }
            self->authenticator->login_async(self->email, self->password, [self](AccountAuthResult result) {
                self->guard([self, result = std::move(result)] {
                    self->password.clear();
                    if (!result.ok) { self->fail("-ERR Authentication failed", "wrong_pass", true); return; }
                    self->user = result.user_id; self->shard = result.shard;
                    // Authentication may still hold the same pool's only lease
                    // during its callback. Queue acquisition after it unwinds.
                    self->queue([self] { self->load_inbox(); });
                });
            });
        });
    }
    void load_inbox() {
        if (session->is_closed()) { fail({}, {}); return; }
        auto pool = router ? router->get_db_pool(static_cast<size_t>(shard)) : nullptr;
        if (!pool) { fail("-ERR Server configuration error", "no_pool"); return; }
        database = repository::make_pop3_db_context(std::move(pool));
        if (!database->valid()) { fail("-ERR Server database unavailable", "no_conn"); return; }
        auto self = this->shared_from_this();
        mailboxes->get_inbox_async(database, user, [self](bool ok, uint64_t id) {
            self->guard([self, ok, id] {
                if (!ok) { self->fail("-ERR Server database unavailable", "inbox_read_error"); return; }
                if (!id) { self->fail("-ERR No INBOX for user", "no_inbox"); return; }
                if (self->session->is_closed()) { self->fail({}, {}); return; }
                self->inbox = id; self->lock_attempted = true;
                self->locks->acquire_async(self->database, {self->user, self->sid}, [self](repository::Pop3LockStatus status) {
                    self->guard([self, status] { self->lock_ready(status); });
                });
            });
        });
    }
    void lock_ready(repository::Pop3LockStatus status) {
        if (status == repository::Pop3LockStatus::database_error) { fail("-ERR Server database unavailable", "lock_error"); return; }
        if (status != repository::Pop3LockStatus::owned) {
            lock_attempted = false;
            auto self = this->shared_from_this();
            session->get_connection().executor()->post([self] {
                if (self->session->is_closed()) return;
                auto* srv = static_cast<MailServerBase*>(self->session->get_server());
                if (srv) if (auto metrics = srv->get_metrics().lock()) metrics->inc_counter("protorelay_pop3_lock_conflict_total", {}, 1);
            });
            fail("-ERR [IN-USE] Mailbox lock busy, try later", "lock_conflict"); return;
        }
        if (session->is_closed()) { fail({}, {}); return; }
        auto self = this->shared_from_this();
        mailboxes->get_messages_async(database, user, inbox, [self](bool ok, std::vector<MailboxMailInfo> rows) mutable {
            self->guard([self, ok, rows = std::move(rows)]() mutable {
                if (!ok) { self->fail("-ERR Cannot load mailbox", "mailbox_read_error"); return; }
                if (self->session->is_closed()) { self->fail({}, {}); return; }
                self->queue([self, rows = std::move(rows)]() mutable { self->measure(std::move(rows)); });
            });
        });
    }
    void measure(std::vector<MailboxMailInfo> rows) {
        std::vector<Pop3Message> messages;
        try {
            auto provider = router ? router->get_storage(static_cast<size_t>(shard)) : nullptr;
            for (auto& row : rows) {
                if (!row.mail_id) continue;
                Pop3Message message{row.mail_id, std::move(row.body_path), 0};
                if (provider) {
                    storage::IoError error; uint64_t size = 0;
                    if (provider->object_size(message.body_path, size, error)) message.size = size;
                }
                messages.push_back(std::move(message));
            }
        } catch (...) { fail("-ERR Cannot load mailbox", "mailbox_storage_error"); return; }
        auto self = this->shared_from_this();
        session->get_connection().executor()->post([self, messages = std::move(messages)]() mutable {
            self->guard([self, messages = std::move(messages)]() mutable { self->install(std::move(messages)); });
        });
    }
    void install(std::vector<Pop3Message> messages) {
        if (session->is_closed()) { fail({}, {}); return; }
        auto* ctx = ctx_of(session);
        if (!ctx) { fail("-ERR Internal: no context", "no_context"); return; }
        ctx->user_id = user; ctx->mailbox_id = inbox; ctx->shard_index = shard;
        ctx->messages = std::move(messages); ctx->deleted.clear(); ctx->session_id = sid;
        ctx->dropped = false; ctx->is_authenticated = true;
        start_heartbeat(session, router, worker, interval, locks);
        uint64_t total = 0;
        for (const auto& message : ctx->messages) total += message.size;
        session->set_paused(false);
        send_line(session, "+OK Mailbox locked and loaded, " + std::to_string(ctx->messages.size()) +
            " messages (" + std::to_string(total) + " octets)");
        session->set_current_state(static_cast<int>(Pop3State::TRANSACTION));
        stopping.store(true); replied.store(true);
        auto* srv = static_cast<MailServerBase*>(session->get_server());
        if (srv) if (auto metrics = srv->get_metrics().lock()) metrics->inc_counter("protorelay_pop3_auth_total", {{"result", "ok"}}, 1);
        session->drain_buffered_commands();
    }
};

template <typename ConnectionType>
struct TraditionalPop3Fsm<ConnectionType>::QuitOperation
    : std::enable_shared_from_this<QuitOperation> {
    std::shared_ptr<pr::SessionBase<ConnectionType>> session;
    std::shared_ptr<router::IShardRouter> router;
    std::shared_ptr<pr::ThreadPoolBase> worker;
    std::shared_ptr<repository::IPop3MailboxRepository> mailboxes;
    std::shared_ptr<repository::IPop3LockRepository> locks;
    repository::Pop3DbContext database;
    repository::Pop3DeletionRequest request;
    repository::Pop3LockKey key;
    int shard = 0;
    std::atomic<bool> releasing{false}, replied{false};
    void finish(bool deleted, bool released) {
        if (replied.exchange(true)) return;
        auto self = this->shared_from_this();
        session->get_connection().executor()->post([self, deleted, released] {
            auto session = self->session;
            if (session->is_closed()) return;
            session->set_paused(false);
            session->set_current_state(static_cast<int>(Pop3State::UPDATE));
            if (deleted && released) { session->set_trace_clean_close(); send_line(session, "+OK Bye"); }
            else send_line(session, "-ERR Unable to complete mailbox update");
            session->close_after_flush();
        });
    }
    void release(bool deleted) {
        if (releasing.exchange(true)) return;
        auto self = this->shared_from_this();
        try {
            if (!database || !database->valid()) { finish(false, false); return; }
            locks->release_async(database, key, [self, deleted](bool released) { self->finish(deleted, released); });
        } catch (...) { finish(false, false); }
    }
    void start() {
        auto self = this->shared_from_this();
        try {
            if (!worker) { finish(false, false); return; }
            worker->post([self] {
                try {
                    self->database = repository::make_pop3_db_context(self->router ? self->router->get_db_pool(static_cast<size_t>(self->shard)) : nullptr);
                    if (!self->database->valid()) { self->finish(false, false); return; }
                    self->mailboxes->apply_deletions_async(self->database, self->request,
                        [self](repository::Pop3DeletionResult result) { self->release(result.ok); });
                } catch (...) { self->release(false); }
            });
        } catch (...) { finish(false, false); }
    }
};

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::handle_pass(std::shared_ptr<pr::SessionBase<ConnectionType>> session) {
    if (!session) return;
    auto* ctx = ctx_of(session);
    if (!ctx) { send_line(session, "-ERR Internal: no context"); return; }
    if (!ctx->is_authenticated && ctx->username.empty()) { send_line(session, "-ERR USER first"); return; }
    auto password = session->get_last_command_args();
    if (password.empty()) { send_line(session, "-ERR PASS requires a password"); return; }
    session->set_paused(true);
    auto operation = std::make_shared<LoginOperation>();
    operation->session = std::move(session); operation->router = m_shardRouter; operation->worker = m_workerThreadPool;
    operation->authenticator = authenticator_; operation->mailboxes = mailbox_repository_; operation->locks = lock_repository_;
    operation->interval = heartbeat_interval_; operation->email = ctx->username; operation->password = std::move(password);
    operation->sid = std::to_string(algorithm::get_snowflake_generator().next_id());
    operation->start();
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::handle_stat(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session) {
    if (!session) return;
    auto* ctx = ctx_of(session);
    if (!ctx) { send_line(session, "-ERR Internal: no context"); return; }
    uint64_t total = 0;
    size_t count = 0;
    for (auto& m : ctx->messages) {
        if (ctx->deleted.count(m.mail_id)) continue;
        total += m.size;
        ++count;
    }
    send_line(session, "+OK " + std::to_string(count) + " " + std::to_string(total));
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::handle_list(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session) {
    if (!session) return;
    auto* ctx = ctx_of(session);
    if (!ctx) { send_line(session, "-ERR Internal: no context"); return; }
    std::string args = session->get_last_command_args();
    if (!args.empty()) {
        uint64_t n = safe_stoull(args);
        if (n < 1 || n > ctx->messages.size()) {
            send_line(session, "-ERR no such message");
            return;
        }
        size_t seen = 0;
        for (auto& m : ctx->messages) {
            if (ctx->deleted.count(m.mail_id)) continue;
            if (++seen == n) {
                send_line(session, "+OK " + std::to_string(n) + " " + std::to_string(m.size));
                return;
            }
        }
        send_line(session, "-ERR no such message");
        return;
    }
    uint64_t total = 0;
    size_t count = 0;
    for (auto& m : ctx->messages) {
        if (ctx->deleted.count(m.mail_id)) continue;
        total += m.size; ++count;
    }
    std::string response = "+OK " + std::to_string(count) + " messages (" +
                           std::to_string(total) + " octets)\r\n";
    size_t n = 0;
    for (auto& m : ctx->messages) {
        if (ctx->deleted.count(m.mail_id)) continue;
        response += std::to_string(++n) + " " + std::to_string(m.size) + "\r\n";
    }
    send_line(session, response + ".\r\n");
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::handle_uidl(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session) {
    if (!session) return;
    auto* ctx = ctx_of(session);
    if (!ctx) { send_line(session, "-ERR Internal: no context"); return; }
    std::string args = session->get_last_command_args();
    if (!args.empty()) {
        uint64_t n = safe_stoull(args);
        if (n < 1 || n > ctx->messages.size()) {
            send_line(session, "-ERR no such message");
            return;
        }
        size_t seen = 0;
        for (auto& m : ctx->messages) {
            if (ctx->deleted.count(m.mail_id)) continue;
            if (++seen == n) {
                send_line(session, "+OK " + std::to_string(n) + " " + std::to_string(m.mail_id));
                return;
            }
        }
        send_line(session, "-ERR no such message");
        return;
    }
    std::string response = "+OK Unique-ID listing follows\r\n";
    size_t n = 0;
    for (auto& m : ctx->messages) {
        if (ctx->deleted.count(m.mail_id)) continue;
        response += std::to_string(++n) + " " + std::to_string(m.mail_id) + "\r\n";
    }
    send_line(session, response + ".\r\n");
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::handle_retr(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session) {
    if (!session) return;
    auto* ctx = ctx_of(session);
    if (!ctx) { send_line(session, "-ERR Internal: no context"); return; }
    std::string args = session->get_last_command_args();
    if (args.empty()) { send_line(session, "-ERR RETR requires a message number"); return; }
    uint64_t n = safe_stoull(args);
    if (n < 1 || n > ctx->messages.size()) {
        send_line(session, "-ERR no such message");
        return;
    }
    size_t seen = 0;
    Pop3Message target;
    bool found = false;
    for (auto& m : ctx->messages) {
        if (ctx->deleted.count(m.mail_id)) continue;
        if (++seen == n) { target = m; found = true; break; }
    }
    if (!found) { send_line(session, "-ERR no such message"); return; }

    // 在 worker 线程读 body + dot-stuffing（避免 io 线程被大邮件卡住）
    session->set_paused(true);
    auto self = session->shared_from_this();
    auto router = m_shardRouter;
    auto worker = m_workerThreadPool;
    int shard = ctx->shard_index;
    if (!worker) {
        session->set_paused(false);
        send_line(session, "-ERR Server not ready");
        return;
    }
    worker->post([self, router, shard, target]() {
        std::string body;
        bool read_ok = false;
        if (router) {
            if (auto provider = router->get_storage(static_cast<size_t>(shard))) {
                storage::IoError err;
                read_ok = provider->read_all(target.body_path, body, err);
            }
        }
        if (!read_ok) {
            self->get_connection().executor()->post([self] {
                if (self->is_closed()) return;
                self->set_paused(false);
                send_line(self, "-ERR Failed to read message body");
                self->drain_buffered_commands();
            });
            return;
        }
        // RFC 1939 §3.3 dot-stuffing：每行以 "." 开头则前缀 "."
        // 逐行转 CRLF；结尾的 '\n' 不产生额外空行（否则终止符前会多一行）
        std::string out;
        out.reserve(body.size() + 64);
        size_t start = 0;
        while (start < body.size()) {
            size_t eol = body.find('\n', start);
            size_t line_end = (eol == std::string::npos) ? body.size() : eol;
            std::string line = body.substr(start, line_end - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty() && line[0] == '.') line.insert(line.begin(), '.');
            out += line;
            out += "\r\n";
            if (eol == std::string::npos) break;
            start = eol + 1;
        }
        self->get_connection().executor()->post([self, target, out = std::move(out)] {
            if (self->is_closed()) return;
            send_line(self, "+OK " + std::to_string(target.size) + " octets\r\n" + out + ".\r\n");
            auto* srv = static_cast<MailServerBase*>(self->get_server());
            if (srv) if (auto metrics = srv->get_metrics().lock())
                metrics->inc_counter("protorelay_pop3_retr_total", {}, 1);
            self->set_paused(false);
            self->drain_buffered_commands();
        });
    });
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::handle_dele(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session) {
    if (!session) return;
    auto* ctx = ctx_of(session);
    if (!ctx) { send_line(session, "-ERR Internal: no context"); return; }
    std::string args = session->get_last_command_args();
    if (args.empty()) { send_line(session, "-ERR DELE requires a message number"); return; }
    uint64_t n = safe_stoull(args);
    if (n < 1 || n > ctx->messages.size()) {
        send_line(session, "-ERR no such message");
        return;
    }
    size_t seen = 0;
    uint64_t target_id = 0;
    bool found = false;
    for (auto& m : ctx->messages) {
        if (ctx->deleted.count(m.mail_id)) continue;
        if (++seen == n) { target_id = m.mail_id; found = true; break; }
    }
    if (!found) { send_line(session, "-ERR no such message"); return; }
    if (ctx->deleted.count(target_id)) {
        send_line(session, "-ERR message already deleted");
        return;
    }
    ctx->deleted.insert(target_id);
    send_line(session, "+OK message " + std::to_string(n) + " deleted");
    if (auto* srv = static_cast<MailServerBase*>(session->get_server())) {
        if (auto m = srv->get_metrics().lock()) {
            m->inc_counter("protorelay_pop3_dele_total", {}, 1);
        }
    }
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::handle_noop(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session) {
    if (!session) return;
    send_line(session, "+OK");
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::handle_rset(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session) {
    if (!session) return;
    auto* ctx = ctx_of(session);
    if (ctx) ctx->deleted.clear();
    send_line(session, "+OK");
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::handle_quit(std::shared_ptr<pr::SessionBase<ConnectionType>> session) {
    if (!session) return;
    auto* ctx = ctx_of(session);
    if (!ctx || !ctx->is_authenticated) {
        session->set_trace_clean_close();
        send_line(session, "+OK Bye");
        session->set_current_state(static_cast<int>(Pop3State::UPDATE));
        session->close_after_flush();
        return;
    }
    session->set_paused(true);
    ctx->dropped = true;
    // Do not rearm a heartbeat (including an already pending renewal) after
    // QUIT has begun. Timer and context ownership remain with the adapter.
    if (auto timer = std::move(ctx->heartbeat_timer)) timer->cancel();
    ctx->heartbeat_handler.reset();
    auto operation = std::make_shared<QuitOperation>();
    operation->session = std::move(session); operation->router = m_shardRouter; operation->worker = m_workerThreadPool;
    operation->mailboxes = mailbox_repository_; operation->locks = lock_repository_;
    operation->request = {ctx->user_id, ctx->mailbox_id, ctx->deleted};
    operation->key = {ctx->user_id, ctx->session_id}; operation->shard = ctx->shard_index;
    operation->start();
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::handle_error(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session) {
    if (!session || session->is_closed()) return;
    // 未知命令/空行 → 回 -ERR，连接保持（用户在 AUTHORIZATION 可重试）
    send_line(session, "-ERR Unknown command");
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::handle_timeout(std::shared_ptr<pr::SessionBase<ConnectionType>> session) {
    if (!session || session->is_closed()) return;
    auto close = [session] { session->get_connection().executor()->post([session] { session->close(); }); };
    auto* ctx = ctx_of(session);
    if (ctx && ctx->is_authenticated && !ctx->session_id.empty() && m_shardRouter && m_workerThreadPool) {
        auto router = m_shardRouter;
        auto locks = lock_repository_;
        const auto shard = ctx->shard_index;
        const repository::Pop3LockKey key{ctx->user_id, ctx->session_id};
        try {
            m_workerThreadPool->post([router, locks, shard, key, close] {
                try {
                    auto context = repository::make_pop3_db_context(router->get_db_pool(static_cast<size_t>(shard)));
                    locks->release_async(context, key, [close](bool) { close(); });
                } catch (...) { close(); }
            });
            return;
        } catch (const std::exception& failure) {
            LOG_SESSION_ERROR("Cannot schedule POP3 timeout cleanup: {}", failure.what());
        }
    }
    close();
}

} // namespace mail_system

#endif // TRADITIONAL_POP3_FSM_TPP
