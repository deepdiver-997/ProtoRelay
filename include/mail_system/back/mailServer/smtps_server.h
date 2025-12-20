#ifndef SMTPS_SERVER_H
#define SMTPS_SERVER_H

#include "mail_system/back/mailServer/server_base.h"
#include "mail_system/back/mailServer/fsm/smtps/smtps_fsm.h"
#include "mail_system/back/mailServer/fsm/smtps/traditional_smtps_fsm.h"
#include "mail_system/back/mailServer/session/smtps_session.h"
#include "mail_system/back/entities/mail.h"

namespace mail_system {

    class SmtpsServer : public ServerBase {
    public:
        SmtpsServer(const ServerConfig& config,
         std::shared_ptr<ThreadPoolBase> ioThreadPool = nullptr,
         std::shared_ptr<ThreadPoolBase> workerThreadPool = nullptr,
         std::shared_ptr<DBPool> dbPool = nullptr);
        virtual ~SmtpsServer() override;

    protected:
        // 处理新连接
        void handle_accept(std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket >>&& ssl_socket,
           const boost::system::error_code& error) override;

        std::string get_free_client_ip();
        void post_to_client(size_t mail_id);
        void post_to_local_client(std::shared_ptr<void> client, std::unique_ptr<mail>&& mail);
        bool inner_ip(const std::string& ip);

        std::shared_ptr<SmtpsFsm> m_fsm;
    };

} // namespace mail_system

#endif // SMTPS_SERVER_H