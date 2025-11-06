#ifndef IMAPS_SERVER_H
#define IMAPS_SERVER_H

#include "server_base.h"
#include "fsm/imaps/imaps_fsm.h"
#include "fsm/imaps/traditional_imaps_fsm.h"
#include "mail_system/back/mailServer/session/imaps_session.h"

namespace mail_system {

    class ImapsServer : public ServerBase {
    public:
        ImapsServer(const ServerConfig& config,
         std::shared_ptr<ThreadPoolBase> ioThreadPool = nullptr,
         std::shared_ptr<ThreadPoolBase> workerThreadPool = nullptr,
         std::shared_ptr<DBPool> dbPool = nullptr);
        virtual ~ImapsServer() override;

    protected:
        // 处理新连接
        void handle_accept(std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket >>&& ssl_socket,
             const boost::system::error_code& error);

        std::shared_ptr<ImapsFsm> m_fsm;
    };

} // namespace mail_system

#endif // IMAPS_SERVER_H