#ifndef IMAPS_SERVER_H
#define IMAPS_SERVER_H

#include "framework/tcp_server_base.h"
#include "mail_system/back/mailServer/fsm/imaps/imap_types.hpp"
#include "mail_system/back/mailServer/fsm/imaps/traditional_imaps_fsm.h"
#include "mail_system/back/mailServer/session/imaps_session.h"
#include "mail_system/back/entities/mail.h"

namespace mail_system {

class ImapsServer : public pr::TcpServerBase<ImapsSession<pr::TcpConnection>,
                                          ImapsSession<pr::SslConnection>> {
public:
    ImapsServer(const ServerConfig& config,
         std::shared_ptr<pr::ThreadPoolBase> ioThreadPool,
         std::shared_ptr<pr::ThreadPoolBase> workerThreadPool,
         std::shared_ptr<router::IShardRouter> shardRouter);
    virtual ~ImapsServer() override;

protected:
    bool should_reject_connection(std::string& reason, const std::string& client_ip = "") const override;

    std::shared_ptr<ImapsSession<pr::TcpConnection>> make_tcp_session(
        std::unique_ptr<pr::TcpConnection> conn, const ListenerConfig& lc) override;
    std::shared_ptr<ImapsSession<pr::SslConnection>> make_ssl_session(
        std::unique_ptr<pr::SslConnection> conn, const ListenerConfig& lc) override;

private:
    std::shared_ptr<TraditionalImapsFsm<pr::TcpConnection>> m_tcp_fsm;
    std::shared_ptr<TraditionalImapsFsm<pr::SslConnection>> m_ssl_fsm;
};

} // namespace mail_system

#endif // IMAPS_SERVER_H
