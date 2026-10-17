#ifndef PR_FRAMEWORK_SERVER_BASE_H
#define PR_FRAMEWORK_SERVER_BASE_H

#include <atomic>
#include <memory>
#include <string>
#include <map>
#include "framework/server_config.h"
#include "framework/db/db_pool.h"
#include "framework/net/dns_resolver.h"
#include "framework/thread_pool/thread_pool_base.h"
#include "framework/intrusion_detector.h"
#include "framework/metrics_server.h"
#include <boost/asio/io_context.hpp>
#include <atomic>
#include <memory>
#include <string>
#include <map>

// 应用层类型只前向声明（include 泄漏清理，2026-10-17）。必须放在 namespace pr
// 之外：它们就是 mail_system 命名空间的应用层类型，放进 pr 会变成
// pr::mail_system::* 假类型（2026-10-18 命名空间翻转时的教训）。成员是
// shared_ptr，拷贝/析构都不需要完整类型；解引用方自己 include 真头。
namespace mail_system { class IMailboxCache; }
namespace mail_system { namespace router { class IShardRouter; } }

namespace pr {

enum class ServerState { Stopped, Running, Pausing, Paused };

inline const char* server_state_to_string(ServerState s) {
    switch (s) {
    case ServerState::Running: return "Running";
    case ServerState::Pausing: return "Pausing";
    case ServerState::Paused:  return "Paused";
    default: return "Stopped";
    }
}

// ================================================================
// ServerBase — 传输层无关的服务器基类
//
//   只负责生命周期、配置、metrics、共享组件。
//   不包含任何 TCP/SSL/accept 逻辑（由 TcpServerBase 等子类提供）。
// ================================================================
class ServerBase {
public:
    // 纯依赖注入（2026-10-18）：只接受现成资源，任一为空/未 start → 抛
    // std::invalid_argument 响亮报错，绝不悄悄自建。装配（DB 池/存储/路由的
    // 具体工厂）在 mail_system::bootstrap::assemble —— 框架不认识任何具体驱动。
    ServerBase(const mail_system::ServerConfig& config,
         std::shared_ptr<ThreadPoolBase> ioThreadPool,
         std::shared_ptr<ThreadPoolBase> workerThreadPool,
         std::shared_ptr<mail_system::router::IShardRouter> shardRouter);
    virtual ~ServerBase();

    ServerBase(const ServerBase&) = delete;
    ServerBase& operator=(const ServerBase&) = delete;
    ServerBase(ServerBase&&) = delete;
    ServerBase& operator=(ServerBase&&) = delete;

    // 非阻塞启动（子类实现具体的监听/连接逻辑）
    virtual void start() = 0;

    // 阻塞当前线程直到收到停止信号
    void run();

    ServerState get_state() const;
    void request_stop() { m_state.store(ServerState::Pausing, std::memory_order_release); }

    // reload 运行时配置
    bool reload_config(const std::string& json_file);

    // io_context (TCP 服务器提供，测试 mock 返回 nullptr)
    virtual std::shared_ptr<boost::asio::io_context> get_io_context() const { return nullptr; }

    // ── 共享成员（所有协议共用） ──────────────────────────────
    std::shared_ptr<ThreadPoolBase> m_ioThreadPool;
    std::shared_ptr<ThreadPoolBase> m_workerThreadPool;
    std::shared_ptr<mail_system::router::IShardRouter> m_shardRouter;
    std::shared_ptr<pr::IDnsResolver> m_dnsResolver;

    void set_mailbox_cache(std::shared_ptr<mail_system::IMailboxCache> cache) { m_mailboxCache = cache; }
    std::shared_ptr<mail_system::IMailboxCache> get_mailbox_cache() const { return m_mailboxCache; }
    std::shared_ptr<mail_system::router::IShardRouter> get_shard_router() const { return m_shardRouter; }
    std::shared_ptr<pr::IDnsResolver> get_dns_resolver() const { return m_dnsResolver; }

    std::shared_ptr<mail_system::IMailboxCache> m_mailboxCache;
    std::string m_domain;
    std::string m_configFilePath;
    std::shared_ptr<mail_system::ServerConfig> m_config;

    // ── 连接计数 ──────────────────────────────────────────────
    std::atomic<size_t> active_connections_{0};
    std::atomic<size_t> connections_total_{0};
    std::atomic<size_t> connections_rejected_total_{0};
    std::atomic<size_t> mails_accepted_total_{0};
    void increment_connection_count();
    void decrement_connection_count();
    void increment_connections_total();
    void increment_connections_rejected();
    void increment_mails_accepted();

    // ── 入侵检测 ──────────────────────────────────────────────
    IntrusionDetector m_intrusionDetector;
    void record_session_end(const std::string& ip, bool authenticated) {
        m_intrusionDetector.record_session(ip, authenticated);
        // 2026-08-27: 未认证会话的 suspicious 计数。push 推迟到 ServerBase 这层
        // 而非 IntrusionDetector 内部，避免动 is_banned 的 const 正确性。
        if (!authenticated) {
            if (auto m = get_metrics().lock()) {
                m->inc_counter("protorelay_intrusion_suspicious_total", {}, 1);
            }
        }
    }
    bool is_ip_banned(const std::string& ip) const {
        auto cfg = std::atomic_load(&m_config);
        if (!cfg->intrusion_detection_enabled || cfg->intrusion_ban_threshold <= 0) return false;
        bool banned = m_intrusionDetector.is_banned(ip);
        // 2026-08-27: ban 命中计数（被拒连接时上层 reject 路径会查 is_ip_banned）。
        if (banned) {
            if (auto m = get_metrics().lock()) {
                m->inc_counter("protorelay_intrusion_bans_total", {}, 1);
            }
        }
        return banned;
    }

    // ── Metrics ───────────────────────────────────────────────
    std::weak_ptr<MetricsServer> get_metrics() const { return m_metricsServer; }
    void refresh_metrics();
    void push_metric_gauge(const std::string& name, const MetricsServer::LabelMap& labels, double v);
    void push_metric_counter(const std::string& name, const MetricsServer::LabelMap& labels, uint64_t v);
    void push_metric_observe(const std::string& name, const MetricsServer::LabelMap& labels, double v);

    [[deprecated]] std::string build_metrics_response() const;
    [[deprecated]] std::string build_status_response() const;

protected:
    virtual void stop(ServerState state = ServerState::Pausing);
    virtual bool should_reject_connection(std::string& reason, const std::string& client_ip = "") const = 0;

    void start_metrics_server();
    void stop_metrics_server();
    std::shared_ptr<MetricsServer> m_metricsServer;

    std::atomic<ServerState> m_state{ServerState::Stopped};
};

} // namespace pr

#endif // PR_FRAMEWORK_SERVER_BASE_H
