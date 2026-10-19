#ifndef POP3_SERVER_H
#define POP3_SERVER_H

#include "framework/tcp_server_base.h"
#include "mail_system/back/mailServer/fsm/pop3/traditional_pop3_fsm.h"
#include "mail_system/back/mailServer/session/pop3_session.h"
#include "framework/net/executor.h"

namespace mail_system {

class Pop3Server : public pr::TcpServerBase<Pop3Session<pr::TcpConnection>,
                                         Pop3Session<pr::SslConnection>> {
public:
    Pop3Server(const ServerConfig& config,
         std::shared_ptr<pr::ThreadPoolBase> ioThreadPool,
         std::shared_ptr<pr::ThreadPoolBase> workerThreadPool,
         std::shared_ptr<router::IShardRouter> shardRouter);
    ~Pop3Server() override;

    void start() override;

protected:
    void stop(pr::ServerState state = pr::ServerState::Pausing) override;

    bool should_reject_connection(std::string& reason, const std::string& client_ip = "") const override;

    std::shared_ptr<Pop3Session<pr::TcpConnection>> make_tcp_session(
        std::unique_ptr<pr::TcpConnection> conn, const ListenerConfig& lc) override;
    std::shared_ptr<Pop3Session<pr::SslConnection>> make_ssl_session(
        std::unique_ptr<pr::SslConnection> conn, const ListenerConfig& lc) override;

private:
    // 锁清扫（v2）：周期回收心跳过期的 pop3_session_lock，防硬崩溃死锁
    void start_lock_sweeper();
    struct LockSweeper;
    std::shared_ptr<LockSweeper> m_lock_sweeper;

    std::shared_ptr<TraditionalPop3Fsm<pr::TcpConnection>> m_tcp_fsm;
    std::shared_ptr<TraditionalPop3Fsm<pr::SslConnection>> m_ssl_fsm;
};

} // namespace mail_system

#endif // POP3_SERVER_H
