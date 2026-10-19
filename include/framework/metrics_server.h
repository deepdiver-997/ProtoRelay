#ifndef PR_FRAMEWORK_METRICS_SERVER_H
#define PR_FRAMEWORK_METRICS_SERVER_H

#include "framework/net/server_transport.h"
#include <array>
#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

namespace pr {

// 线程安全的指标存储 + HTTP 端点
// 组件通过 push 接口提交指标，/metrics 和 /status 端点直接渲染存储数据
//
// 使用方式:
//   ServerBase 持有 shared_ptr<MetricsServer>
//   各组件持有 weak_ptr<MetricsServer>，析构时自动失效
//   组件在事件发生时 push: if (auto m = metrics.lock()) m->inc("x", ...);
class MetricsServer : public std::enable_shared_from_this<MetricsServer> {
public:
    using LabelMap = std::map<std::string, std::string>;

    using ReloadHandler    = std::function<std::string()>;
    using RefreshProvider   = std::function<void()>;  // render 前刷新慢变化指标

    MetricsServer(std::shared_ptr<net::IServerTransportProvider> provider,
                  uint16_t port, const std::string& bind_address,
                  ReloadHandler reload_handler = nullptr,
                  RefreshProvider refresh = nullptr)
        : provider_(std::move(provider)), port_(port), bound_port_(0),
          bind_address_(bind_address), reload_handler_(std::move(reload_handler)),
          refresh_provider_(std::move(refresh)), running_(false) {}

    ~MetricsServer() { stop(); }

    MetricsServer(const MetricsServer&) = delete;
    MetricsServer& operator=(const MetricsServer&) = delete;

    // 返回实际绑定的端口（start 之后）。port=0 时由 OS 分配，调用方从
    // bound_port() 读到真实端口（方便测试用 port=0 + 读 bound_port 避端口冲突）。
    uint16_t bound_port() const { return bound_port_; }

    // ── 指标注入接口 (线程安全) ──────────────────────────────────

    // 值瞬时覆盖上一次
    void set_gauge(const std::string& name, const LabelMap& labels, double val) {
        std::unique_lock lock(mutex_);
        gauges_[make_key(name, labels)] = {build_help(name), val};
    }

    // 单调递增
    void inc_counter(const std::string& name, const LabelMap& labels, uint64_t delta = 1) {
        std::unique_lock lock(mutex_);
        auto& c = counters_[make_key(name, labels)];
        if (c.help.empty()) c.help = build_help(name);
        c.value += delta;
    }

    // 记录一次观测值 (累计 sum + count，用于计算 rate/avg)
    void observe(const std::string& name, const LabelMap& labels, double value) {
        std::unique_lock lock(mutex_);
        std::string key = make_key(name, labels);
        auto& h = histograms_[key];
        if (h.help.empty()) h.help = build_help(name);
        h.sum   += value;
        h.count += 1;
    }

    // ── HTTP 服务 ───────────────────────────────────────────────

    // 公开的渲染接口：复用 build_metrics() 内部逻辑（refresh + 锁 + Prometheus 格式）。
    // 生产用 HTTP /metrics；测试用本方法不启 HTTP 端点就能验证 push 的数据真到内部 map。
    std::string scrape_text() { return build_metrics(); }

    void start() {
        if (running_.load()) return;
        if (!provider_) throw std::invalid_argument("metrics HTTP requires a server transport provider");
        auto weak = weak_from_this();
        if (weak.expired()) throw std::logic_error("running metrics HTTP requires shared ownership");
        net::ServerTransportConfig config;
        config.address = bind_address_;
        config.listeners.emplace_back(ListenerType::TCP, port_);
        transport_ = provider_->make_server_transport(config);
        if (!transport_ || transport_->listeners().size() != 1)
            throw std::runtime_error("metrics backend did not create a listener");
        bound_port_ = transport_->listeners().front().port;
        running_.store(true);
        try {
            transport_->start([weak](std::unique_ptr<IConnection> connection, const ListenerConfig&) {
                if (auto self = weak.lock()) self->handle_request(std::move(connection));
            });
        } catch (...) { running_.store(false); throw; }
    }

    void stop() {
        running_.store(false);
        if (transport_) transport_->stop();
        // Wait for any active render/reload callback before the server which
        // supplied those callbacks can destroy its state.
        std::lock_guard<std::mutex> requests(requests_mutex_);
        std::vector<std::weak_ptr<IConnection>> connections;
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            connections.swap(connections_);
        }
        for (auto& weak : connections) if (auto connection = weak.lock())
            connection->executor()->post([connection] { connection->close(); });
    }

private:
    // ── 渲染 ────────────────────────────────────────────────────

    struct GaugeEntry    { std::string help; double value = 0; };
    struct CounterEntry  { std::string help; uint64_t value = 0; };
    struct HistogramEntry { std::string help; double sum = 0; uint64_t count = 0; };

    // key = "name{label=val,...}" — 保证唯一性
    static std::string make_key(const std::string& name, const LabelMap& labels) {
        if (labels.empty()) return name;
        std::string k = name + "{";
        bool first = true;
        for (auto& [lk, lv] : labels) {
            if (!first) k += ",";
            k += lk + "=\"" + lv + "\"";
            first = false;
        }
        k += "}";
        return k;
    }

    static std::string build_help(const std::string& name) {
        return "Metric " + name;
    }

    // 解析 key 为 name + labels
    static std::pair<std::string, std::string> split_key(const std::string& key) {
        auto pos = key.find('{');
        if (pos == std::string::npos) return {key, ""};
        return {key.substr(0, pos), key.substr(pos)};
    }

    std::string build_metrics() {
        if (refresh_provider_) refresh_provider_();
        std::shared_lock lock(mutex_);
        std::string out;
        out.reserve(4096);
        for (auto& [key, g] : gauges_) {
            auto [name, labels] = split_key(key);
            out += "# HELP " + name + " " + g.help + "\n";
            out += "# TYPE " + name + " gauge\n";
            out += name;
            if (!labels.empty()) out += labels;
            out += " " + std::to_string(static_cast<int64_t>(g.value)) + "\n";
        }
        for (auto& [key, c] : counters_) {
            auto [name, labels] = split_key(key);
            out += "# HELP " + name + " " + c.help + "\n";
            out += "# TYPE " + name + " counter\n";
            out += name;
            if (!labels.empty()) out += labels;
            out += " " + std::to_string(c.value) + "\n";
        }
        for (auto& [key, h] : histograms_) {
            auto [name, labels] = split_key(key);
            out += "# HELP " + name + "_sum " + h.help + "\n";
            out += "# TYPE " + name + "_sum gauge\n";
            out += name + "_sum";
            if (!labels.empty()) out += labels;
            out += " " + std::to_string(h.sum) + "\n";
            out += "# HELP " + name + "_count " + h.help + "\n";
            out += "# TYPE " + name + "_count gauge\n";
            out += name + "_count";
            if (!labels.empty()) out += labels;
            out += " " + std::to_string(h.count) + "\n";
        }
        return out;
    }

    std::string build_status() {
        std::shared_lock lock(mutex_);
        std::string out = "{\n";
        bool first = true;
        for (auto& [key, g] : gauges_) {
            if (!first) out += ",\n";
            auto [name, labels] = split_key(key);
            out += "  \"" + key + "\": " + std::to_string(g.value);
            first = false;
        }
        for (auto& [key, c] : counters_) {
            if (!first) out += ",\n";
            out += "  \"" + key + "\": " + std::to_string(c.value);
            first = false;
        }
        out += "\n}\n";
        return out;
    }

    // ── HTTP ────────────────────────────────────────────────────

    void handle_request(std::shared_ptr<IConnection> connection) {
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            connections_.erase(std::remove_if(connections_.begin(), connections_.end(),
                [](const auto& weak) { return weak.expired(); }), connections_.end());
            connections_.push_back(connection);
        }
        auto weak = weak_from_this();
        connection->executor()->post([weak, connection] {
            if (auto self = weak.lock(); self && self->running_.load())
                self->read_headers(connection, std::make_shared<std::string>());
            else connection->close();
        });
    }
    void read_headers(std::shared_ptr<IConnection> connection, std::shared_ptr<std::string> request) {
        auto buffer = std::make_shared<std::array<char, 4096>>();
        auto weak = weak_from_this();
        connection->async_read(net::buffer(buffer->data(), buffer->size()),
            [weak, connection, request, buffer](const std::error_code& error, size_t size) {
                auto self = weak.lock();
                if (!self) { connection->close(); return; }
                std::lock_guard<std::mutex> request_lock(self->requests_mutex_);
                if (!self->running_.load() || error || size == 0) { connection->close(); return; }
                request->append(buffer->data(), size);
                if (request->size() > 64 * 1024) { connection->close(); return; }
                if (request->find("\r\n\r\n") == std::string::npos)
                    self->read_headers(connection, request);
                else self->respond(connection, *request);
            });
    }
    void respond(std::shared_ptr<IConnection> connection, const std::string& request) {
        std::string method, path;
        size_t s = request.find(' '), e = request.find(' ', s + 1);
        if (s != std::string::npos && e != std::string::npos) {
            method = request.substr(0, s);
            path   = request.substr(s + 1, e - s - 1);
        }

        std::string status, content_type, body;
        if (path == "/metrics" || path == "/") {
            status = "200 OK";
            content_type = "text/plain; version=0.0.4";
            body = build_metrics();
        } else if (path == "/status") {
            status = "200 OK";
            content_type = "application/json";
            body = build_status();
        } else if (method == "POST" && path == "/reload" && reload_handler_) {
            std::string result = reload_handler_();
            status = (result.empty() || result == "OK") ? "200 OK" : "500 Internal Server Error";
            content_type = "text/plain";
            body = status == "200 OK" ? "OK" : result;
        } else if (path == "/health" || path == "/health/live") {
            status = "200 OK"; content_type = "text/plain"; body = "OK";
        } else if (path == "/health/ready") {
            body = build_metrics();
            status = body.empty() ? "503 Service Unavailable" : "200 OK";
            content_type = "text/plain";
        } else {
            status = "404 Not Found"; content_type = "text/plain"; body = "Not Found";
        }

        auto resp = std::make_shared<std::string>();
        resp->reserve(256 + body.size());
        resp->append("HTTP/1.1 ").append(status).append("\r\n");
        resp->append("Content-Type: ").append(content_type).append("\r\n");
        resp->append("Content-Length: ").append(std::to_string(body.size())).append("\r\n");
        resp->append("Connection: close\r\n\r\n");
        resp->append(body);

        connection->async_write(net::buffer(*resp),
            [connection, resp](const std::error_code&, size_t) { connection->close(); });
    }

    std::shared_ptr<net::IServerTransportProvider> provider_;
    std::unique_ptr<net::IServerTransport> transport_;
    std::mutex connections_mutex_;
    std::mutex requests_mutex_;
    std::vector<std::weak_ptr<IConnection>> connections_;
    uint16_t port_;
    uint16_t bound_port_;   // start() 后从后端监听器读出
    std::string bind_address_;
    ReloadHandler reload_handler_;
    RefreshProvider refresh_provider_;
    std::atomic<bool> running_;

    mutable std::shared_mutex mutex_;
    std::map<std::string, GaugeEntry>    gauges_;
    std::map<std::string, CounterEntry>  counters_;
    std::map<std::string, HistogramEntry> histograms_;
};

} // namespace pr

#endif
