#include "mail_system/back/mailServer/imaps_server.h"
#include <iostream>

namespace mail_system {

ImapsServer::ImapsServer(const ServerConfig& config,
     std::shared_ptr<ThreadPoolBase> ioThreadPool,
      std::shared_ptr<ThreadPoolBase> workerThreadPool,
       std::shared_ptr<DBPool> dbPool)
        : ServerBase(config, ioThreadPool, workerThreadPool, dbPool) {
    m_fsm = std::make_shared<TraditionalImapsFsm>(m_ioThreadPool, m_workerThreadPool, m_dbPool);
}

ImapsServer::~ImapsServer() {
    // 确保服务器停止
    stop();
}

void ImapsServer::handle_accept(std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket> >&& ssl_socket, const boost::system::error_code& error) {
    // 创建会话
    std::shared_ptr<ImapsSession> session = std::make_shared<ImapsSession>(this, std::move(ssl_socket), m_fsm);
    if (!error) {
        try {
            std::cout << "New IMAPS connection from " << session->get_client_ip() << std::endl;

            // 启动会话
            if(!m_workerThreadPool->is_running()) {
                session->start();
            }
            else {
                m_workerThreadPool->post([session]() {
                    session->start();
                });
            }

            std::cout << "num after start: " << session.use_count() << std::endl;
        }
        catch (const std::exception& e) {
            std::cerr << "Error starting IMAPS session: " << e.what() << std::endl;
        }
    }
    else {
        std::cerr << "IMAPS accept error: " << error.message() << std::endl;
    }

}

} // namespace mail_system