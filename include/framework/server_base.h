#ifndef PR_FRAMEWORK_SERVER_BASE_H
#define PR_FRAMEWORK_SERVER_BASE_H

#include <atomic>
#include <memory>
#include <string>
#include <map>
#include "framework/server_config_base.h"
#include "framework/net/executor.h"
#include "framework/thread_pool/thread_pool_base.h"
#include "framework/intrusion_detector.h"
#include "framework/metrics_server.h"

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
    using Config = pr::ServerConfig;
    // Injected runtimes are owned and stopped by the application composition root.
    ServerBase(const Config& config,
               std::shared_ptr<ThreadPoolBase> ioThreadPool,
               std::shared_ptr<ThreadPoolBase> workerThreadPool);
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
    virtual bool reload_config(const std::string& json_file);

    // Backend-neutral scheduling for server-owned maintenance tasks.
    std::shared_ptr<net::IExecutor> get_executor() const;

    // ── 共享成员（所有协议共用） ──────────────────────────────
    std::shared_ptr<ThreadPoolBase> m_ioThreadPool;
    std::shared_ptr<ThreadPoolBase> m_workerThreadPool;
    std::string m_configFilePath;
    std::shared_ptr<Config> m_config;
    // A single virtual snapshot keeps sessions on their application's current
    // immutable configuration without exposing application types to the framework.
    virtual std::shared_ptr<const Config> config_snapshot() const {
        return std::atomic_load(&m_config);
    }

    // ── 连接计数 ──────────────────────────────────────────────
    std::atomic<size_t> active_connections_{0};
    std::atomic<size_t> connections_total_{0};
    std::atomic<size_t> connections_rejected_total_{0};
    void increment_connection_count();
    void decrement_connection_count();
    void increment_connections_total();
    void increment_connections_rejected();

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
        auto cfg = config_snapshot();
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
    virtual void refresh_metrics();
    void push_metric_gauge(const std::string& name, const MetricsServer::LabelMap& labels, double v);
    void push_metric_counter(const std::string& name, const MetricsServer::LabelMap& labels, uint64_t v);
    void push_metric_observe(const std::string& name, const MetricsServer::LabelMap& labels, double v);

    [[deprecated]] std::string build_metrics_response() const;
    [[deprecated]] std::string build_status_response() const;

protected:
    bool validate_reload_structure(const Config& replacement) const;
    virtual void stop(ServerState state = ServerState::Pausing);
    virtual bool should_reject_connection(std::string& reason, const std::string& client_ip = "") const = 0;

    void start_metrics_server();
    void stop_metrics_server();
    std::shared_ptr<MetricsServer> m_metricsServer;

    std::atomic<ServerState> m_state{ServerState::Stopped};
};

} // namespace pr

#endif // PR_FRAMEWORK_SERVER_BASE_H
