#ifndef TRADITIONAL_POP3_FSM_H
#define TRADITIONAL_POP3_FSM_H

#include "framework/session_base.h"
#include "mail_system/back/common/account_authentication.h"
#include "framework/fsm/fsm_engine.h"
#include "mail_system/back/common/auth_cache.h"
#include "mail_system/back/router/i_shard_router.h"
#include "framework/thread_pool/thread_pool_base.h"
#include "mail_system/back/mailServer/fsm/pop3/pop3_types.hpp"
#include "mail_system/back/mailServer/fsm/pop3/pop3_desc.h"
#include "mail_system/back/repository/pop3_mailbox_repository.h"
#include "mail_system/back/repository/pop3_lock_repository.h"
#include <chrono>
#include <cstdint>
#include <functional>
#include <set>
#include <stdexcept>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mail_system {

// Holds the fsm engine instead of inheriting pr::FsmBase: dispatch, guard
// replies and the transition write points now live in the engine (design
// §3.2/§4 step 1). Protocol behavior (handlers, operations, facades) is
// unchanged; the guard/handler tables moved to make_pop3_desc + register_.
template <typename ConnectionType>
class TraditionalPop3Fsm {
public:
    using Engine = pr::fsm::Engine<ConnectionType, Pop3State, Pop3Event>;

    std::shared_ptr<AuthCache> m_authCache = std::make_shared<AuthCache>();
    // 锁心跳续约周期（默认 60s）。测试可改短以在有限时间内观察续约/失锁。
    std::chrono::milliseconds heartbeat_interval_{std::chrono::seconds(60)};

protected:
    std::shared_ptr<pr::ThreadPoolBase> m_ioThreadPool;
    std::shared_ptr<pr::ThreadPoolBase> m_workerThreadPool;
    std::shared_ptr<router::IShardRouter> m_shardRouter;
    std::shared_ptr<IAccountAuthenticator> authenticator_;
    std::shared_ptr<repository::IPop3MailboxRepository> mailbox_repository_ = repository::make_sql_pop3_mailbox_repository();
    std::shared_ptr<repository::IPop3LockRepository> lock_repository_ = repository::make_sql_pop3_lock_repository();
    Engine engine_;

public:
    TraditionalPop3Fsm(
        std::shared_ptr<pr::ThreadPoolBase> io_thread_pool,
        std::shared_ptr<pr::ThreadPoolBase> worker_thread_pool,
        std::shared_ptr<router::IShardRouter> shard_router
    ) : m_ioThreadPool(io_thread_pool),
        m_workerThreadPool(worker_thread_pool),
        m_shardRouter(std::move(shard_router)),
        engine_(make_pop3_desc()) {
        authenticator_ = std::make_shared<AccountAuthenticationService>(
            m_shardRouter, m_authCache, m_workerThreadPool);
        register_handlers();
    }

    ~TraditionalPop3Fsm() = default;
    void set_authenticator(std::shared_ptr<IAccountAuthenticator> authenticator) {
        if (authenticator) authenticator_ = std::move(authenticator);
    }

    void set_mailbox_repository(std::shared_ptr<repository::IPop3MailboxRepository> value) {
        if (!value) throw std::invalid_argument("POP3 mailbox repository is required");
        mailbox_repository_ = std::move(value);
    }
    void set_lock_repository(std::shared_ptr<repository::IPop3LockRepository> value) {
        if (!value) throw std::invalid_argument("POP3 lock repository is required");
        lock_repository_ = std::move(value);
    }

    // ========== 事件派发入口（session 调） ==========
    void process_event(
        std::shared_ptr<pr::SessionBase<ConnectionType>> session,
        Pop3Event event);

    // 从 session 的 next_event_ 取事件后派发（仿 SMTP auto_process_event）
    void auto_process_event(
        std::shared_ptr<pr::SessionBase<ConnectionType>> session);

    // Compatibility facades delegate to default repositories. Helpers accepting
    // a bare lease borrow its pool: the caller must keep that pool alive until
    // completion. Production chains pass an owning Pop3DbContext instead.
    static void auth_user_async(const std::shared_ptr<IAccountAuthenticator>& authenticator,
                                const std::string& mail_address,
                                const std::string& password,
                                std::function<void(bool ok, uint64_t user_id, int shard)> cb);

    // 取 INBOX id（box_type=1）。失败 cb(0)。
    static void get_inbox_id_async(std::shared_ptr<ScopedConnection> conn, uint64_t user_id,
                                   std::function<void(uint64_t)> cb);

    // Return the established mailbox snapshot (including pre-existing deleted
    // flags). POP3 DELE/RSET flags remain local until successful QUIT.
    static void get_inbox_mails_async(std::shared_ptr<ScopedConnection> conn,
                                      uint64_t mailbox_id, uint64_t user_id,
                                      std::function<void(std::vector<Pop3Message>)> cb);

    // 取 shard 的 storage provider（仿 imaps_fsm.tpp get_storage）
    std::shared_ptr<storage::IStorageProvider> get_storage(int shard) {
        return m_shardRouter ? m_shardRouter->get_storage(static_cast<size_t>(shard)) : nullptr;
    }

    // 申请 mailbox 锁。cb(false) 表示所有权未确认（冲突或数据库错误）。
    static void acquire_lock_async(std::shared_ptr<ScopedConnection> conn, uint64_t user_id,
                                   const std::string& session_id,
                                   std::function<void(bool)> cb);

    // Legacy release facade: completion-only. Production uses the typed result.
    static void release_lock_async(std::shared_ptr<ScopedConnection> conn, uint64_t user_id,
                                   const std::string& session_id, std::function<void()> cb);

    // 续约锁心跳（v2）：条件 UPDATE last_heartbeat + verify 所有权。
    // cb(false) = 无法确认所有权（失锁或数据库错误），调用方应关闭会话。
    static void renew_lock_heartbeat_async(const std::shared_ptr<router::IShardRouter>& router,
                                           uint64_t user_id, const std::string& session_id,
                                           int shard, std::function<void(bool)> cb);

    // 清扫过期锁（v2 sweeper Worker 周期调用），每分片委托仓储。
    static bool sweep_expired_locks(const std::shared_ptr<router::IShardRouter>& router);

    // Commit local DELE flags, then remove marked links. Failures stop the chain.
    static void apply_deletions_async(std::shared_ptr<ScopedConnection> conn, uint64_t user_id,
                                      uint64_t mailbox_id,
                                      const std::set<uint64_t>& deleted_mail_ids,
                                      std::function<void(bool)> cb);

    // 写一行（带 CRLF）到 session，跨线程安全（通过 session->do_async_write）。
    // 协议规范：`+OK text\r\n` / `-ERR text\r\n`。
    static void send_line(std::shared_ptr<pr::SessionBase<ConnectionType>> session,
                          const std::string& line);

    // 取 session 的 Pop3Context 裸指针（每个 session 自带 context_ 成员，
    // 通过 get_context() 拿到 pr::SessionBase 的协议层 context —— 但 pr::SessionBase
    // 接口是 void*，本 FSM 强转为 Pop3Context*）。
    static Pop3Context* ctx_of(std::shared_ptr<pr::SessionBase<ConnectionType>> session);

    // ========== 状态/事件名（调试 / 日志） ==========
    static std::string get_state_name(Pop3State s);
    static std::string get_event_name(Pop3Event e);

private:
    struct LoginOperation;
    struct QuitOperation;
    void register_handlers();

    // ========== 锁心跳（v2） ==========
    // 在 io_context 上启动递归续约定时器；handler 只捕获 weak 引用打破自环，
    // 会话 close() 时取消。静态：不依赖 FSM 实例（router/worker/interval 显式传入）。
    static void start_heartbeat(std::shared_ptr<pr::SessionBase<ConnectionType>> session,
                                std::shared_ptr<router::IShardRouter> router,
                                std::shared_ptr<pr::ThreadPoolBase> worker,
                                std::chrono::milliseconds interval,
                                std::shared_ptr<repository::IPop3LockRepository> locks);

    // ========== 状态处理器 ==========
    void handle_init_connect(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_capa(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_user(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_pass(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_stat(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_list(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_uidl(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_retr(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_dele(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_noop(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_rset(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_quit(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_error(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_timeout(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
};

} // namespace mail_system

#endif // TRADITIONAL_POP3_FSM_H
