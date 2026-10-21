#ifndef TRADITIONAL_IMAPS_FSM_H
#define TRADITIONAL_IMAPS_FSM_H

#include "framework/session_base.h"
#include "mail_system/back/common/account_authentication.h"
#include "framework/fsm_base.h"
#include "mail_system/back/db/db_pool.h"
#include "mail_system/back/repository/mailbox_repository.h"
#include "mail_system/back/repository/imap_mutation_repository.h"
#include "mail_system/back/repository/imap_message_repository.h"
#include "mail_system/back/repository/imap_folder_repository.h"
#include "mail_system/back/common/imap_append_service.h"
#include <stdexcept>
#include "framework/thread_pool/thread_pool_base.h"
#include "mail_system/back/mailServer/fsm/imaps/imap_types.hpp"
#include "mail_system/back/mailServer/fsm/imaps/imap_utils.h"
#include "mail_system/back/common/logger.h"
#include "mail_system/back/common/lru_cache.h"
#include "mail_system/back/common/auth_cache.h"
#include "mail_system/back/common/bcrypt.h"
#include "mail_system/back/storage/i_storage_provider.h"
#include "mail_system/back/router/i_shard_router.h"
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <regex>
#include <string>
#include <sstream>
#include <unordered_map>
#include <iomanip>
#include <utility>
#include <vector>

namespace mail_system {

// FETCH 邮箱列表缓存条目。⚠ 必须在命名空间级（不能嵌在 FSM 模板里）：
// TCP/SSL 两个模板实例的 MailboxListCache 得是同一个类型，imaps_server 才能
// 共享一份实例——否则一边的写命令失效不了另一边的缓存。
// 失效语义（三层）：
//   1. 写命令显式失效——STORE/EXPUNGE/COPY/MOVE/APPEND/DELETE 完成时按
//      (user, mailbox) 精确失效（flags 变化与删信不推进 uidnext，必须显式）；
//   2. TTL 兜底——跨节点/跨协议（SMTP 投递、POP3 DELE）改库本进程看不到，
//      靠 TTL 过期回源，新邮件可见性延迟 ≤ TTL；
//   3. 大列表跳过——超上限的邮箱不缓存（单条可到 MB 级，防 LRU 被撑爆）。
struct MailboxListEntry {
    std::vector<MailboxMailInfo> mails;
};

template <typename ConnectionType>
class TraditionalImapsFsm : public pr::FsmBase<ConnectionType, ImapState, ImapEvent> {
public:
    using MailboxStatsCache = LruCache<std::string, MailboxCacheEntry>;
    using MailboxListCache = LruCache<std::string, MailboxListEntry>;

    // 列表缓存的容量/单列表邮件数上限/TTL（imaps_server 按此配置注入）
    static constexpr size_t kMailboxListCacheCapacity = 2048;
    static constexpr size_t kMailboxListCacheMaxMails = 1000;
    static constexpr std::chrono::seconds kMailboxListCacheTtl{2};

    std::shared_ptr<AuthCache> m_authCache = std::make_shared<AuthCache>();

protected:
    std::shared_ptr<pr::ThreadPoolBase> m_ioThreadPool;
    std::shared_ptr<pr::ThreadPoolBase> m_workerThreadPool;
    std::shared_ptr<router::IShardRouter> m_shardRouter;
    std::shared_ptr<IAccountAuthenticator> authenticator_;
    std::shared_ptr<repository::IMailboxRepository> m_mailboxRepository = repository::make_sql_mailbox_repository();
    std::shared_ptr<repository::IImapMutationRepository> m_mutationRepository = repository::make_sql_imap_mutation_repository();
    std::shared_ptr<repository::IImapMessageRepository> m_messageRepository = repository::make_sql_imap_message_repository();
    std::shared_ptr<repository::IImapFolderRepository> m_folderRepository = repository::make_sql_imap_folder_repository();
    std::shared_ptr<IImapAppendService> m_appendService;
    std::shared_ptr<MailboxStatsCache> m_mailboxStatsCache;
    std::shared_ptr<MailboxListCache> m_mailboxListCache;
    // SELECT/STATUS 统计的 single-flight：同一 (user,mailbox) 只允许一个回源查询链
    // 在途，其余并发 SELECT 挂到等待列表上（owner 完成后统一通知），防缓存
    // miss/stale 时 N 个连接各自查库打爆数据库。
    struct StatsFlight {
        std::vector<std::function<void(MailboxCacheEntry, bool, bool)>> waiters;
    };
    std::unordered_map<std::string, std::shared_ptr<StatsFlight>> m_statsFlights;
    std::mutex m_statsFlightsMutex;

public:
    TraditionalImapsFsm(
        std::shared_ptr<pr::ThreadPoolBase> io_thread_pool,
        std::shared_ptr<pr::ThreadPoolBase> worker_thread_pool,
        std::shared_ptr<router::IShardRouter> shard_router
    ) : m_ioThreadPool(io_thread_pool),
        m_workerThreadPool(worker_thread_pool),
        m_shardRouter(std::move(shard_router)) {
        authenticator_ = std::make_shared<AccountAuthenticationService>(
            m_shardRouter, m_authCache, m_workerThreadPool);
        m_appendService = make_imap_append_service(m_workerThreadPool, m_messageRepository);
        init_transition_table();
        init_state_handlers();
    }

    ~TraditionalImapsFsm() override = default;
    void set_authenticator(std::shared_ptr<IAccountAuthenticator> authenticator) {
        if (authenticator) authenticator_ = std::move(authenticator);
    }


    // Configure before serving sessions, like the shared cache dependencies.
    void set_mailbox_repository(std::shared_ptr<repository::IMailboxRepository> value) {
        if (!value) throw std::invalid_argument("IMAP mailbox repository is required");
        m_mailboxRepository = std::move(value);
    }
    std::shared_ptr<repository::IMailboxRepository> get_mailbox_repository() const { return m_mailboxRepository; }

    void set_mutation_repository(std::shared_ptr<repository::IImapMutationRepository> value) {
        if (!value) throw std::invalid_argument("IMAP mutation repository is required");
        m_mutationRepository = std::move(value);
    }
    std::shared_ptr<repository::IImapMutationRepository> get_mutation_repository() const { return m_mutationRepository; }

    // Configure before serving sessions. Install a custom APPEND service last;
    // changing its metadata repository rebuilds the default service.
    void set_message_repository(std::shared_ptr<repository::IImapMessageRepository> value) {
        if (!value) throw std::invalid_argument("IMAP message repository is required");
        m_messageRepository = std::move(value);
        m_appendService = make_imap_append_service(m_workerThreadPool, m_messageRepository);
    }
    std::shared_ptr<repository::IImapMessageRepository> get_message_repository() const { return m_messageRepository; }
    void set_folder_repository(std::shared_ptr<repository::IImapFolderRepository> value) {
        if (!value) throw std::invalid_argument("IMAP folder repository is required");
        m_folderRepository = std::move(value);
    }
    std::shared_ptr<repository::IImapFolderRepository> get_folder_repository() const { return m_folderRepository; }
    void set_append_service(std::shared_ptr<IImapAppendService> value) {
        if (!value) throw std::invalid_argument("IMAP APPEND service is required");
        m_appendService = std::move(value);
    }
    std::shared_ptr<IImapAppendService> get_append_service() const { return m_appendService; }

    void set_mailbox_stats_cache(std::shared_ptr<MailboxStatsCache> cache) { m_mailboxStatsCache = cache; }
    std::shared_ptr<MailboxStatsCache> get_mailbox_stats_cache() const { return m_mailboxStatsCache; }

    void set_mailbox_list_cache(std::shared_ptr<MailboxListCache> cache) { m_mailboxListCache = cache; }
    std::shared_ptr<MailboxListCache> get_mailbox_list_cache() const { return m_mailboxListCache; }

    // 写命令完成后调用：精确失效 (user, mailbox) 的列表与统计缓存（未注入时空操作）
    void invalidate_mailbox_list(uint64_t user_id, uint64_t mailbox_id);

    std::shared_ptr<ScopedConnection> acquire_connection(int shard) {
        auto pool = m_shardRouter ? m_shardRouter->get_db_pool(static_cast<size_t>(shard)) : nullptr;
        if (!pool) {
            LOG_DATABASE_ERROR("No DB pool for shard {}", shard);
            return ScopedConnection::invalid();
        }
        // A ScopedConnection borrows its DBPool. The lease itself must keep the
        // pool alive even if the session/FSM/router dies before a DB callback.
        struct OwnedLease {
            std::shared_ptr<DBPool> pool;
            std::shared_ptr<ScopedConnection> connection;
        };
        auto connection = pool->acquire_connection();
        auto owner = std::make_shared<OwnedLease>(OwnedLease{std::move(pool), std::move(connection)});
        return std::shared_ptr<ScopedConnection>(owner, owner->connection.get());
    }

    std::shared_ptr<storage::IStorageProvider> get_storage(int shard) {
        return m_shardRouter->get_storage(static_cast<size_t>(shard));
    }

    // 事件处理入口
    void process_event(
        std::shared_ptr<pr::SessionBase<ConnectionType>> session,
        ImapEvent event,
        const std::string& tag);

    // 自动处理（从 session 获取当前 pending 事件）
    void auto_process_event(std::shared_ptr<pr::SessionBase<ConnectionType>> session);

    // ========== 状态/事件名称查询 ==========
    static std::string get_state_name(ImapState state) {
        static const std::unordered_map<ImapState, std::string> names = {
            {ImapState::INIT, "INIT"},
            {ImapState::NOT_AUTHENTICATED, "NOT_AUTHENTICATED"},
            {ImapState::AUTHENTICATED, "AUTHENTICATED"},
            {ImapState::SELECTED, "SELECTED"},
            {ImapState::LOGOUT, "LOGOUT"},
            {ImapState::CLOSED, "CLOSED"}
        };
        auto it = names.find(state);
        return it != names.end() ? it->second : "UNKNOWN_STATE";
    }

    static std::string get_event_name(ImapEvent event) {
        static const std::unordered_map<ImapEvent, std::string> names = {
            {ImapEvent::CONNECT, "CONNECT"},
            {ImapEvent::CAPABILITY, "CAPABILITY"},
            {ImapEvent::LOGIN, "LOGIN"},
            {ImapEvent::AUTHENTICATE, "AUTHENTICATE"},
            {ImapEvent::LOGOUT, "LOGOUT"},
            {ImapEvent::SELECT, "SELECT"},
            {ImapEvent::EXAMINE, "EXAMINE"},
            {ImapEvent::CREATE, "CREATE"},
            {ImapEvent::DELETE, "DELETE"},
            {ImapEvent::RENAME, "RENAME"},
            {ImapEvent::SUBSCRIBE, "SUBSCRIBE"},
            {ImapEvent::UNSUBSCRIBE, "UNSUBSCRIBE"},
            {ImapEvent::LIST, "LIST"},
            {ImapEvent::LSUB, "LSUB"},
            {ImapEvent::IMAP_STATUS, "STATUS"},
            {ImapEvent::APPEND, "APPEND"},
            {ImapEvent::CHECK, "CHECK"},
            {ImapEvent::CLOSE, "CLOSE"},
            {ImapEvent::EXPUNGE, "EXPUNGE"},
            {ImapEvent::SEARCH, "SEARCH"},
            {ImapEvent::FETCH, "FETCH"},
            {ImapEvent::STORE, "STORE"},
            {ImapEvent::COPY, "COPY"},
            {ImapEvent::MOVE, "MOVE"},
            {ImapEvent::UID, "UID"},
            {ImapEvent::NOOP, "NOOP"},
            {ImapEvent::IDLE, "IDLE"},
            {ImapEvent::DONE, "DONE"},
            {ImapEvent::STARTTLS, "STARTTLS"},
            {ImapEvent::ERROR, "ERROR"},
            {ImapEvent::TIMEOUT, "TIMEOUT"}
        };
        auto it = names.find(event);
        return it != names.end() ? it->second : "UNKNOWN_EVENT";
    }

    // ========== 可注入应用仓储 facade ==========
    // 协议只消费领域结果。仓储持有连接租约并负责数据库方言与列映射；
    // 内联/延迟完成均可，协议状态恢复必须经会话 executor 串行化。

    // 用户认证（复用和 SMTP 相同的 users 表）
    // 登录认证（查 DB + bcrypt）。bcrypt 几十~几百 ms 纯 CPU，调用方
    // 必须在 worker 线程发起本函数（见 handle_login）。
    static void auth_user_async(const std::shared_ptr<IAccountAuthenticator>& authenticator,
                                const std::string& mail_address,
                                const std::string& password,
                                std::function<void(bool ok, uint64_t user_id, int shard)> cb);

    // 邮箱列表（LIST/LSUB）。失败 cb(false, {})。
    void get_mailboxes_async(std::shared_ptr<ScopedConnection> conn, uint64_t user_id,
                                    std::function<void(bool,
                                        std::vector<MailboxSummary>)> cb);

    // 按名称找邮箱 id（含 INBOX 兜底）。失败 cb(0)。
    void find_mailbox_id_async(std::shared_ptr<ScopedConnection> conn, uint64_t user_id,
                                      const std::string& mailbox_name,
                                      std::function<void(uint64_t)> cb);

    // 邮箱内邮件列表（FETCH/SEARCH/STORE/EXPUNGE/COPY/MOVE 用）。
    void get_mailbox_mails_async(std::shared_ptr<ScopedConnection> conn,
                                        uint64_t mailbox_id, uint64_t user_id,
                                        std::function<void(bool, std::vector<MailboxMailInfo>)> cb);

    void get_mail_info_async(std::shared_ptr<ScopedConnection> conn, uint64_t mail_id,
                                    std::function<void(bool, MailboxMailInfo)> cb);

    void get_mail_sender_async(std::shared_ptr<ScopedConnection> conn, uint64_t mail_id,
                                      std::function<void(std::string)> cb);

    void get_mail_recipients_async(std::shared_ptr<ScopedConnection> conn, uint64_t mail_id,
                                          std::function<void(std::vector<std::string>)> cb);

    void get_user_email_async(std::shared_ptr<ScopedConnection> conn, uint64_t user_id,
                                     std::function<void(std::string)> cb);

    // 缓存感知的邮箱统计（CPS）。缓存命中且未过期时 cb 同步触发。
    // 本函数在 miss/stale 时自行回源并写回缓存，无需调用方再刷。
    void get_mailbox_stats_cached_async(std::shared_ptr<ScopedConnection> conn,
                                        uint64_t user_id, uint64_t mailbox_id,
                                        std::function<void(MailboxCacheEntry, bool, bool)> cb);

    void get_mailbox_count_async(std::shared_ptr<ScopedConnection> conn,
                                        uint64_t mailbox_id, uint64_t user_id,
                                        std::function<void(size_t)> cb);

    void get_mailbox_unseen_count_async(std::shared_ptr<ScopedConnection> conn,
                                               uint64_t mailbox_id, uint64_t user_id,
                                               std::function<void(size_t)> cb);

    void get_mailbox_uidnext_async(std::shared_ptr<ScopedConnection> conn,
                                          uint64_t mailbox_id, uint64_t user_id,
                                          std::function<void(uint64_t)> cb);

    // 通用 IMAP 响应写回
    void send_untagged(std::shared_ptr<pr::SessionBase<ConnectionType>> session, const std::string& data);
    // static：只触碰 session，可在无 this 的回调（worker/异步续作）里调用
    static void send_tagged(std::shared_ptr<pr::SessionBase<ConnectionType>> session,
                            const std::string& tag,
                            const std::string& status,
                            const std::string& message);
    void send_continuation(std::shared_ptr<pr::SessionBase<ConnectionType>> session,
                           const std::string& message);

private:
    // 初始化
    void init_transition_table();
    void init_state_handlers();

    // pr::FsmBase hooks
    using BaseFsm = pr::FsmBase<ConnectionType, ImapState, ImapEvent>;
    using Handler = typename BaseFsm::Handler;
    bool is_terminal_state(ImapState s) const override;
    void on_invalid_transition(ImapState s, ImapEvent e,
        std::shared_ptr<pr::SessionBase<ConnectionType>> session) override;
    void on_handler_not_found(ImapState s, ImapEvent e,
        std::shared_ptr<pr::SessionBase<ConnectionType>> session) override;
    void invoke_handler(Handler& h,
        std::shared_ptr<pr::SessionBase<ConnectionType>> session) override;

    // ========== 状态处理器 ==========
    void handle_init_connect(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_capability(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_login(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_authenticate(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    // LOGIN 与 AUTHENTICATE PLAIN 共用的验证链：auto-domain 后 pause，把
    // 「查 DB/缓存 + bcrypt 验密码」整体丢 worker 线程池，tagged 响应在回调补发
    void start_auth_verify(std::shared_ptr<pr::SessionBase<ConnectionType>> session,
                           const std::string& tag,
                           std::string username, std::string password,
                           const char* ok_text, const char* fail_text);
    // 解码 SASL PLAIN（authzid NUL authcid NUL passwd）后走 start_auth_verify；
    // 解码失败/结构不对 → NO
    void verify_sasl_plain(std::shared_ptr<pr::SessionBase<ConnectionType>> session,
                           const std::string& tag, const std::string& b64,
                           const char* ok_text);
    void handle_logout(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_select(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_examine(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_list(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_status(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_fetch(std::shared_ptr<pr::SessionBase<ConnectionType>> session,
                      bool is_uid = false);
    // 拿到邮件列表后的 FETCH 组装/驱动（DB 慢路径回调与列表缓存快路径共用）。
    // 原为 handle_fetch 回调体内联代码，提取成函数让缓存命中路径零改动复用。
    void fetch_from_mails(std::shared_ptr<pr::SessionBase<ConnectionType>> session,
                          std::string tag, bool is_uid,
                          std::string seq_set, std::string attrs,
                          std::vector<MailboxMailInfo> mails);

    // FETCH 的续作链状态与驱动器（定义在 .tpp）。读取走 provider 的
    // async 接口：本地内联，远程后端由装饰器投递到 worker —— 逐封
    // size/正文读取不再阻塞 io 线程。static：回调里无 this。
    // 续作链迭代化（不递归）：每封一个 shared atomic alive，size/body 两个异步读
    // 共享。完成本封的最后一步走 fetch_continue——inline 回调（外层 fetch_drive
    // 帧仍在）→ 外层循环 continue；deferred 回调（外层已返回）→ 由回调驱动下一封。
    // 避免本地 storage 内联回调导致的逐封栈递归（大邮箱 >~200 封爆栈）。
    struct FetchContext;
    static void fetch_drive(std::shared_ptr<pr::SessionBase<ConnectionType>> session,
                            std::shared_ptr<FetchContext> ctx);
    // 完成本封剩余 item 后的续作：inline 时外层循环续，deferred 时驱动下一封
    static void fetch_continue(std::shared_ptr<pr::SessionBase<ConnectionType>> session,
                               std::shared_ptr<FetchContext> ctx,
                               std::shared_ptr<std::atomic<bool>> alive);
    // 本封的 envelope + 正文阶段。返回 true = 已发起异步正文读取（回调里 fetch_continue）；
    // false = 正文已同步就绪并完成本封。
    static bool fetch_after_size(std::shared_ptr<pr::SessionBase<ConnectionType>> session,
                                 std::shared_ptr<FetchContext> ctx,
                                 std::shared_ptr<std::atomic<bool>> alive);
    // 用已就绪的正文完成当前邮件剩余 item（纯数据推进，不触碰 session）
    static void fetch_complete_mail_with_body(FetchContext& ctx,
                                              std::string body_content);
    static void fetch_finalize(std::shared_ptr<pr::SessionBase<ConnectionType>> session,
                               std::shared_ptr<FetchContext> ctx);
    void handle_store(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_expunge(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_close(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_noop(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_check(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_create(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_delete(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_rename(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_subscribe(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_unsubscribe(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_lsub(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_append(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_search(std::shared_ptr<pr::SessionBase<ConnectionType>> session,
                       bool is_uid = false);
    void handle_uid(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_starttls(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_copy(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_move(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_copy_move(std::shared_ptr<pr::SessionBase<ConnectionType>> session, bool is_move);
    void handle_idle(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_done(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_error(std::shared_ptr<pr::SessionBase<ConnectionType>> session);
    void handle_timeout(std::shared_ptr<pr::SessionBase<ConnectionType>> session);

    // 辅助函数
};

} // namespace mail_system


#endif // TRADITIONAL_IMAPS_FSM_H
