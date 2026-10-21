#include "mail_system/back/mailServer/outbound_server.h"
#include "mail_system/back/submission/outbound_config_factory.h"
#include "mail_system/back/mailServer/smtps_server.h"
#include "framework/connection/ssl_connection.h"
#include "framework/connection/tcp_connection.h"
#include "mail_system/back/mailServer/outbound/cares_dns_resolver.h"
#include "mail_system/back/mailServer/outbound/outbound_config.h"
#include "mail_system/back/common/logger.h"
#include <iostream>
#include <memory>
#include <netdb.h>
#include <arpa/inet.h>

namespace mail_system {

SmtpsServer::SmtpsServer(const ServerConfig& config,
     std::shared_ptr<pr::ThreadPoolBase> ioThreadPool,
      std::shared_ptr<pr::ThreadPoolBase> wokerThreadPool,
       std::shared_ptr<router::IShardRouter> shardRouter)
        : TcpServerBase(config, ioThreadPool, wokerThreadPool, shardRouter) {
    auto cfg = std::atomic_load(&m_config);

    // DNS resolver (共享基础资源，入站验证+出站投递+DNSBL共用)
    m_dnsResolver = std::make_shared<outbound::CaresDnsResolver>();
    // 2026-08-27: 注入 metrics。DNS 解析慢/失败的可见性靠这个 weak_ptr 推。
    // m_dnsResolver 是 shared_ptr<IDnsResolver> 接口类型，set_metrics 是
    // CaresDnsResolver 派生类扩展；本构造器知道创建的是 CaresDnsResolver 实例，
    // 用 static_pointer_cast 安全下转。
    std::static_pointer_cast<outbound::CaresDnsResolver>(m_dnsResolver)
        ->set_metrics(get_metrics());

    // SMTP 专用：持久化队列和出站投递
    if (m_shardRouter && cfg->use_database) {
        if (!m_persistentQueue) {
            m_persistentQueue = std::make_shared<persist_storage::PersistentQueue>(
                m_shardRouter, m_workerThreadPool);
            m_persistentQueue->set_local_domain(m_domain);
            persist_storage::PersistentQueuePressureConfig pressure_config;
            pressure_config.max_inflight_mails = cfg->persist_max_inflight_mails;
            pressure_config.min_available_memory_mb = cfg->persist_min_available_memory_mb;
            pressure_config.min_db_available_connections = cfg->persist_min_db_available_connections;
            m_persistentQueue->set_pressure_config(pressure_config);
            // 出站 max_attempts：config → outbox 行（入队时写列，重试/DEAD 判断用）
            m_persistentQueue->set_max_attempts(cfg->outbound_max_attempts);
            m_persistentQueue->inject_metrics(get_metrics());
            LOG_SERVER_INFO("PersistentQueue created for SMTP server");
        }

        // 新 outbound 引擎：DB 路径下默认启用，PQ 在 has_external_recipient
        // 命中时会调 submit() 触发投递。
        if (!m_outboundServer) {
            m_outboundServer = std::make_shared<outbound::OutboundServer>(this);
            // 2026-08-27: 注入 metrics weak_ptr（同 PersistentQueue::inject_metrics 模式）。
            // OutboundServer 内部 5 个指标（attempts/delivered/deferred/bounced/connect_duration）
            // 全部通过这个 weak_ptr push，server 析构后自动失效。
            m_outboundServer->set_metrics(get_metrics());
            // 把 server_config 平铺字段组装成 OutboundConfig（含 static_routes）
            m_outboundServer->set_config(submission::outbound_config(*cfg));
        }
        m_persistentQueue->set_outbound_server(m_outboundServer);
        LOG_SERVER_INFO("OutboundServer (new engine) wired into PersistentQueue");
    } else {
        LOG_SERVER_WARN("No database pool — SMTP outbound delivery disabled");
    }

    m_tcp_fsm = std::make_shared<TraditionalSmtpsFsm<pr::TcpConnection>>(
        m_ioThreadPool, m_workerThreadPool, m_persistentQueue, m_shardRouter);
    m_ssl_fsm = std::make_shared<TraditionalSmtpsFsm<pr::SslConnection>>(
        m_ioThreadPool, m_workerThreadPool, m_persistentQueue, m_shardRouter);

    // DB 路径下 m_outboundServer 已被 wire 到 PersistentQueue；启动它开始拉 outbox
    if (m_outboundServer) {
        m_outboundServer->start();
        LOG_SERVER_INFO("OutboundServer (new engine) started");
    }
}

SmtpsServer::~SmtpsServer() {
    stop();
}

void SmtpsServer::stop(pr::ServerState state) {
    if (m_outboundServer) { m_outboundServer->stop(); LOG_SERVER_INFO("OutboundServer stopped"); }
    if (m_persistentQueue) {
        m_persistentQueue->shutdown();
        m_persistentQueue.reset();
        LOG_SERVER_INFO("PersistentQueue shutdown");
    }
    TcpServerBase::stop(state);
}

std::shared_ptr<SmtpsSession<pr::TcpConnection>> SmtpsServer::make_tcp_session(
    std::unique_ptr<pr::TcpConnection> conn, const ListenerConfig& lc)
{
    auto session = std::make_shared<SmtpsSession<pr::TcpConnection>>(
        this, std::move(conn), m_tcp_fsm);
    session->set_listener_config(lc);
    return session;
}

std::shared_ptr<SmtpsSession<pr::SslConnection>> SmtpsServer::make_ssl_session(
    std::unique_ptr<pr::SslConnection> conn, const ListenerConfig& lc)
{
    auto session = std::make_shared<SmtpsSession<pr::SslConnection>>(
        this, std::move(conn), m_ssl_fsm);
    session->set_listener_config(lc);
    return session;
}

bool SmtpsServer::should_reject_connection(std::string& reason, const std::string& client_ip) const {
    auto cfg = std::atomic_load(&m_config);

    // DNSBL 反垃圾检查
    if (!cfg->perf_mode && cfg->dnsbl_enabled && !client_ip.empty()) {
        std::string reversed;
        in_addr address{};
        if (inet_pton(AF_INET, client_ip.c_str(), &address) == 1) {
            const auto* bytes = reinterpret_cast<const unsigned char*>(&address.s_addr);
            reversed = std::to_string(bytes[3]) + "." + std::to_string(bytes[2]) + "." +
                       std::to_string(bytes[1]) + "." + std::to_string(bytes[0]);
        }

        if (!reversed.empty()) {
            std::string query = reversed + ".zen.spamhaus.org";
            struct addrinfo hints = {};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            struct addrinfo* result = nullptr;

            if (getaddrinfo(query.c_str(), nullptr, &hints, &result) == 0 && result) {
                char ip_str[INET_ADDRSTRLEN] = {};
                auto* sin = reinterpret_cast<struct sockaddr_in*>(result->ai_addr);
                inet_ntop(AF_INET, &sin->sin_addr, ip_str, sizeof(ip_str));
                std::string resolved = ip_str;
                freeaddrinfo(result);

                if (resolved.compare(0, 7, "127.0.0") == 0) {
                    reason = "DNSBL listed (" + resolved + "): " + query;
                    return true;
                }
            }
        }
    }

    if (cfg->maxConnections > 0 &&
        active_connections_.load(std::memory_order_relaxed) >= cfg->maxConnections) {
        reason = "max connections reached";
        return true;
    }

    if (m_persistentQueue &&
        cfg->persist_max_inflight_mails > 0 &&
        m_persistentQueue->inflight_count() >= cfg->persist_max_inflight_mails) {
        reason = "persist inflight limit reached";
        return true;
    }

    return false;
}

} // namespace mail_system
