#ifndef SMTPS_SERVER_H
#define SMTPS_SERVER_H

#include "framework/tcp_server_base.h"
#include "mail_system/back/mailServer/fsm/smtps/smtps_types.hpp"
#include "mail_system/back/mailServer/fsm/smtps/traditional_smtps_fsm.h"
#include "mail_system/back/mailServer/session/smtps_session.h"
#include "mail_system/back/entities/mail.h"
#include "mail_system/back/persist_storage/persistent_queue.h"
#include "mail_system/back/mailServer/outbound_server.h"

namespace mail_system {

class SmtpsServer : public pr::TcpServerBase<SmtpsSession<pr::TcpConnection>,
                                          SmtpsSession<pr::SslConnection>> {
public:
    SmtpsServer(const ServerConfig& config,
         std::shared_ptr<pr::ThreadPoolBase> ioThreadPool,
         std::shared_ptr<pr::ThreadPoolBase> workerThreadPool,
         std::shared_ptr<router::IShardRouter> shardRouter);
    virtual ~SmtpsServer() override;

protected:
    void stop(pr::ServerState state = pr::ServerState::Pausing) override;
    bool should_reject_connection(std::string& reason, const std::string& client_ip = "") const override;

    // pr::TcpServerBase 工厂
    std::shared_ptr<SmtpsSession<pr::TcpConnection>> make_tcp_session(
        std::unique_ptr<pr::TcpConnection> conn, const ListenerConfig& lc) override;
    std::shared_ptr<SmtpsSession<pr::SslConnection>> make_ssl_session(
        std::unique_ptr<pr::SslConnection> conn, const ListenerConfig& lc) override;

private:
    std::shared_ptr<TraditionalSmtpsFsm<pr::TcpConnection>> m_tcp_fsm;
    std::shared_ptr<TraditionalSmtpsFsm<pr::SslConnection>> m_ssl_fsm;

public:
    std::shared_ptr<persist_storage::PersistentQueue> m_persistentQueue;

    // 新 outbound 引擎（可选注入，为 null 则不启用出站投递）
    std::shared_ptr<outbound::OutboundServer> m_outboundServer;
};

} // namespace mail_system

#endif // SMTPS_SERVER_H
