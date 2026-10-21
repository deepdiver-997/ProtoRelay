#pragma once

#include "mail_system/back/mailServer/outbound/outbound_config.h"
#include "mail_system/back/mailServer/outbound/outbound_types.hpp"
#include "mail_system/back/smtp_client/client_session.h"
#include <atomic>
#include <queue>

namespace pr { class MetricsServer; }

namespace mail_system::outbound {

// 出站策略适配器：排队、MX failover、有限连接重试、worker 上加载/签名。
// SMTP 协议和连接状态由 ClientSession 唯一实现，不再继承服务端 SessionBase。
class OutboundSmtpSession : public std::enable_shared_from_this<OutboundSmtpSession> {
public:
    using CompletionCb = std::function<void(uint64_t, smtp::Result)>;
    using WorkerPost = std::function<void(std::function<void()>)>;
    OutboundSmtpSession(smtp::TransportProvider transports, std::shared_ptr<pr::net::IExecutor> executor,
                        WorkerPost worker_post,
                        std::string host, int port, std::string helo,
                        std::weak_ptr<pr::MetricsServer> metrics = {});
    void set_outbound_config(OutboundConfig config);
    void set_mx_pool(std::vector<std::string> pool);
    void set_completion_cb(CompletionCb complete);
    void submit(std::unique_ptr<MailDeliveryTask> task);
    void close(); // shutdown 不回写未完成租约，由 outbox lease 恢复
    size_t queue_size() const { return pending_.load(); }
    bool has_active_task() const { return active_.load(); }
    bool is_connected() const { return state() == SessionState::CONNECTED || state() == SessionState::DELIVERING; }
    SessionState state() const { return state_.load(); }
private:
    void pump();
    void prepare();
    void connect();
    void send();
    void complete(smtp::Result result);
    void idle();
    void quit();
    smtp::TransportProvider transports_;
    WorkerPost worker_post_;
    std::shared_ptr<pr::net::IExecutor> executor_;
    std::shared_ptr<pr::net::ITimer> timer_;
    std::string host_, helo_;
    int port_;
    OutboundConfig config_;
    CompletionCb complete_;
    std::weak_ptr<pr::MetricsServer> metrics_;
    std::vector<std::string> mx_pool_;
    size_t mx_index_ = 0;
    int retries_ = 0, sent_ = 0;
    bool stopped_ = false;
    std::queue<std::unique_ptr<MailDeliveryTask>> queue_;
    std::unique_ptr<MailDeliveryTask> task_;
    std::shared_ptr<const std::string> payload_;
    std::shared_ptr<smtp::ClientSession> client_;
    std::atomic<size_t> pending_{0};
    std::atomic<bool> active_{false};
    std::atomic<SessionState> state_{SessionState::INIT};
};
} // namespace mail_system::outbound
