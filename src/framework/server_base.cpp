#include "framework/server_base.h"
#include "framework/net/server_transport.h"
#include "framework/log/log_macros.h"
#include <iostream>
#include <fstream>
#include <stdexcept>

namespace pr {

std::shared_ptr<net::IExecutor> ServerBase::get_executor() const {
    auto provider = std::dynamic_pointer_cast<net::IExecutorProvider>(m_ioThreadPool);
    if (!provider) throw std::invalid_argument("ServerBase: IO runtime must provide executors");
    auto executor = provider->next_executor();
    if (!executor) throw std::runtime_error("ServerBase: IO runtime returned an empty executor");
    return executor;
}

ServerBase::ServerBase(const Config& config,
                       std::shared_ptr<ThreadPoolBase> ioThreadPool,
                       std::shared_ptr<ThreadPoolBase> workerThreadPool)
    : m_ioThreadPool(std::move(ioThreadPool)),
      m_workerThreadPool(std::move(workerThreadPool)),
      m_config(std::make_shared<Config>(config)) {
    if (!m_ioThreadPool || !m_workerThreadPool)
        throw std::invalid_argument("ServerBase: IO and worker runtimes are required");
    if (!m_ioThreadPool->is_running() || !m_workerThreadPool->is_running())
        throw std::invalid_argument("ServerBase: thread pools must be started before construction");
    LOG_SERVER_INFO("Server initialized with {} listener(s)", config.listeners.size());
    m_state = ServerState::Paused;
}

ServerBase::~ServerBase() {
    try { stop(); } catch (...) {}
}

void ServerBase::run() {
    while (m_state.load() == ServerState::Running || m_state.load() == ServerState::Paused)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void ServerBase::stop(ServerState state) {
    auto cur = m_state.load();
    if (cur != ServerState::Running && cur != ServerState::Pausing) return;
    m_state.store(state);

    stop_metrics_server();

    // 纯 DI（2026-10-18）：io/worker 池是调用方注入的资源，所有权在装配方
    // （bootstrap / main / 测试 fixture），ServerBase 析构与 stop() 都不碰它们
    // ——共享注入场景（mailServer 合并入口两 server 共池、测试多用例共池）下，
    // 第一个 server 析构就把池停掉，后续构造直接拿到"已停止"的池。
    // 池的关闭顺序归装配方：main 持 Assembled 资源，作用域结束时自行 stop。

    m_intrusionDetector.persist();
    LOG_SERVER_INFO("Server stopped");
}

ServerState ServerBase::get_state() const { return m_state.load(); }

void ServerBase::start_metrics_server() {
    auto cfg = config_snapshot();
    if (!cfg->metrics_enabled || m_metricsServer) return;
    try {
        auto provider = std::dynamic_pointer_cast<net::IServerTransportProvider>(m_ioThreadPool);
        m_metricsServer = std::make_shared<MetricsServer>(
            std::move(provider),
            cfg->metrics_port, cfg->metrics_bind_address,
            [this]() -> std::string {
                if (m_configFilePath.empty()) return "config file path not set";
                return reload_config(m_configFilePath) ? "OK" : "reload failed";
            },
            [this]() { refresh_metrics(); });
        m_metricsServer->start();
        refresh_metrics();
        LOG_SERVER_INFO("Metrics server started on {}:{}",
                        cfg->metrics_bind_address, cfg->metrics_port);
    } catch (const std::exception& e) {
        LOG_SERVER_ERROR("Failed to start metrics server: {}", e.what());
        m_metricsServer.reset();
    }
}

void ServerBase::stop_metrics_server() {
    if (m_metricsServer) { m_metricsServer->stop(); m_metricsServer.reset(); }
}

void ServerBase::push_metric_gauge(const std::string& name,
                                     const MetricsServer::LabelMap& labels, double v) {
    if (auto m = m_metricsServer) m->set_gauge(name, labels, v);
}
void ServerBase::push_metric_counter(const std::string& name,
                                       const MetricsServer::LabelMap& labels, uint64_t v) {
    if (auto m = m_metricsServer) m->inc_counter(name, labels, v);
}
void ServerBase::push_metric_observe(const std::string& name,
                                       const MetricsServer::LabelMap& labels, double v) {
    if (auto m = m_metricsServer) m->observe(name, labels, v);
}

void ServerBase::increment_connection_count() {
    active_connections_.fetch_add(1, std::memory_order_relaxed);
    push_metric_gauge("protorelay_active_connections", {}, active_connections_.load());
}
void ServerBase::decrement_connection_count() {
    active_connections_.fetch_sub(1, std::memory_order_relaxed);
    push_metric_gauge("protorelay_active_connections", {}, active_connections_.load());
}
// 2026-08-27 fix: 之前把 fetch_add 返回的累计值 v 当 delta 传给 inc_counter
// （c.value += delta），结果 N 次调用后 counter map 累加 1+2+...+N = N*(N+1)/2
// （三角形数），不是 N。inc_counter 语义是"additive delta"，应传 1。
// 修法：fetch_add 后传 delta=1，保留 ServerBase 自己的 atomic 累计值供其他用途。
void ServerBase::increment_connections_total() {
    connections_total_.fetch_add(1, std::memory_order_relaxed);
    push_metric_counter("protorelay_connections_total", {}, 1);
}
void ServerBase::increment_connections_rejected() {
    connections_rejected_total_.fetch_add(1, std::memory_order_relaxed);
    push_metric_counter("protorelay_connections_rejected_total", {}, 1);
}
void ServerBase::refresh_metrics() {}

bool ServerBase::validate_reload_structure(const Config& replacement) const {
    auto old = config_snapshot();
    if (old->address != replacement.address || old->listeners.size() != replacement.listeners.size()) {
        LOG_SERVER_WARN("Config reload rejected: structural fields changed (requires restart)");
        return false;
    }
    for (size_t i = 0; i < old->listeners.size(); ++i) {
        if (old->listeners[i].port != replacement.listeners[i].port ||
            old->listeners[i].type != replacement.listeners[i].type) {
            LOG_SERVER_WARN("Config reload rejected: listener structure changed");
            return false;
        }
    }
    return true;
}

bool ServerBase::reload_config(const std::string& json_file) {
    m_configFilePath = json_file;
    Config replacement = *config_snapshot();
    if (!replacement.loadFromFile(json_file) || !validate_reload_structure(replacement)) return false;
    std::atomic_store(&m_config, std::make_shared<Config>(std::move(replacement)));
    Logger::get_instance().set_level(Logger::string_to_level(config_snapshot()->log_level));
    LOG_SERVER_INFO("Config reloaded: log_level={}", config_snapshot()->log_level);
    return true;
}

// deprecated stubs
std::string ServerBase::build_metrics_response() const { return ""; }
std::string ServerBase::build_status_response() const { return ""; }

} // namespace pr
