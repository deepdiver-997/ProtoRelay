#include "mail_system/back/mailServer/pop3_server.h"
#include "framework/connection/ssl_connection.h"
#include "framework/connection/tcp_connection.h"
#include "mail_system/back/common/logger.h"
#include <chrono>
#include <atomic>
#include <memory>

namespace mail_system {

namespace {
// 锁清扫周期：每 5min 回收心跳过期(>5min)的锁。与续约周期(60s)留足余量。
constexpr int kLockSweepIntervalSeconds = 300;
} // namespace

// Owns maintenance resources, never the server. Even an already-ready success
// callback may outlive stop()/~Pop3Server(), so no handler captures a raw this.
struct Pop3Server::LockSweeper : std::enable_shared_from_this<LockSweeper> {
    std::shared_ptr<pr::net::IExecutor> executor;
    std::shared_ptr<pr::net::ITimer> timer;
    std::shared_ptr<pr::ThreadPoolBase> worker;
    std::shared_ptr<router::IShardRouter> router;
    std::atomic<bool> stopped{false};

    void start() {
        executor->post([self = shared_from_this()] { self->arm(); });
    }
    void stop() {
        stopped.store(true, std::memory_order_release);
        auto current = timer;
        executor->post([current] { current->cancel(); });
    }
    void arm() {
        if (stopped.load(std::memory_order_acquire)) return;
        try {
            timer->expires_after(std::chrono::seconds(kLockSweepIntervalSeconds));
            std::weak_ptr<LockSweeper> weak = shared_from_this();
            timer->async_wait([weak](const std::error_code& error) {
                auto self = weak.lock();
                if (!self || self->stopped.load(std::memory_order_acquire)) return;
                if (error) {
                    self->stopped.store(true, std::memory_order_release);
                    LOG_SERVER_ERROR("POP3 lock sweeper timer failed: {}", error.message());
                    return;
                }
                self->arm();
                try {
                    self->worker->post([router = self->router] {
                        if (!TraditionalPop3Fsm<pr::TcpConnection>::sweep_expired_locks(router))
                            LOG_SERVER_WARN("POP3 expired-lock sweep failed");
                    });
                } catch (const std::exception& failure) {
                    LOG_SERVER_ERROR("Cannot schedule POP3 expired-lock sweep: {}", failure.what());
                }
            });
        } catch (const std::exception& failure) {
            stopped.store(true, std::memory_order_release);
            LOG_SERVER_ERROR("Cannot arm POP3 lock sweeper: {}", failure.what());
        }
    }
};

Pop3Server::Pop3Server(const ServerConfig& config,
     std::shared_ptr<pr::ThreadPoolBase> ioThreadPool,
      std::shared_ptr<pr::ThreadPoolBase> workerThreadPool,
       std::shared_ptr<router::IShardRouter> shardRouter)
        : TcpServerBase(config, ioThreadPool, workerThreadPool, shardRouter) {
    m_tcp_fsm = std::make_shared<TraditionalPop3Fsm<pr::TcpConnection>>(
        m_ioThreadPool, m_workerThreadPool, m_shardRouter);
    m_ssl_fsm = std::make_shared<TraditionalPop3Fsm<pr::SslConnection>>(
        m_ioThreadPool, m_workerThreadPool, m_shardRouter);
    LOG_SERVER_INFO("POP3 server initialized, SSL fsm={}, TCP fsm={}",
                  m_ssl_fsm ? "ready" : "null",
                  m_tcp_fsm ? "ready" : "null");
}

Pop3Server::~Pop3Server() {
    stop();
}

void Pop3Server::start() {
    pr::TcpServerBase<Pop3Session<pr::TcpConnection>, Pop3Session<pr::SslConnection>>::start();
    start_lock_sweeper();
}

void Pop3Server::stop(pr::ServerState state) {
    // Mark stopped immediately; cancel on the owning executor. A queued expiry
    // cannot access a destroyed server or start another maintenance task.
    if (auto sweeper = std::move(m_lock_sweeper)) sweeper->stop();
    pr::TcpServerBase<Pop3Session<pr::TcpConnection>, Pop3Session<pr::SslConnection>>::stop(state);
}

void Pop3Server::start_lock_sweeper() {
    if (m_lock_sweeper) return;
    auto sweeper = std::make_shared<LockSweeper>();
    sweeper->executor = get_executor();
    sweeper->timer = sweeper->executor->make_timer();
    sweeper->worker = m_workerThreadPool;
    sweeper->router = m_shardRouter;
    sweeper->start();
    m_lock_sweeper = std::move(sweeper);
    LOG_SERVER_INFO("POP3 lock sweeper started (every {}s)", kLockSweepIntervalSeconds);
}

std::shared_ptr<Pop3Session<pr::TcpConnection>> Pop3Server::make_tcp_session(
    std::unique_ptr<pr::TcpConnection> conn, const ListenerConfig& lc)
{
    (void)lc;
    return std::make_shared<Pop3Session<pr::TcpConnection>>(
        this, std::move(conn), m_tcp_fsm);
}

std::shared_ptr<Pop3Session<pr::SslConnection>> Pop3Server::make_ssl_session(
    std::unique_ptr<pr::SslConnection> conn, const ListenerConfig& lc)
{
    (void)lc;
    return std::make_shared<Pop3Session<pr::SslConnection>>(
        this, std::move(conn), m_ssl_fsm);
}

bool Pop3Server::should_reject_connection(std::string& reason, const std::string&) const {
    auto cfg = std::atomic_load(&m_config);
    if (cfg->maxConnections > 0 &&
        active_connections_.load(std::memory_order_relaxed) >= cfg->maxConnections) {
        reason = "max connections reached";
        return true;
    }
    return false;
}

} // namespace mail_system
