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
    // 转发到unique_ptr版本的实现
    handle_accept_unique(std::move(ssl_socket), error);
}

void ImapsServer::handle_accept_unique(std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>&& ssl_socket, const boost::system::error_code& error) {
    if (!error) {
        try {
            // 创建会话并使用unique_ptr管理
            create_session_unique(std::move(ssl_socket));
        }
        catch (const std::exception& e) {
            std::cerr << "Error starting IMAPS session (unique_ptr version): " << e.what() << std::endl;
        }
    }
    else {
        std::cerr << "IMAPS accept error (unique_ptr version): " << error.message() << std::endl;
    }
}

void ImapsServer::create_session_unique(std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>&& ssl_socket) {
    // 创建unique_ptr会话并立即启动
    auto session = std::make_unique<ImapsSession>(this, std::move(ssl_socket), m_fsm);
    auto* session_ptr = session.get(); // 保存裸指针供后续使用
    
    std::cout << "New IMAPS connection from " << session_ptr->get_client_ip() << " (unique_ptr version)" << std::endl;
    
    // 启动会话
    if(!m_workerThreadPool->is_running()) {
        session_ptr->start_unique();
        // session在作用域结束时自动销毁
    }
    else {
        // 将session移动到lambda中，确保生命周期
        m_workerThreadPool->post([session = std::move(session), session_ptr]() {
            session_ptr->start_unique();
            // session在lambda结束时自动销毁
        });
    }
}

} // namespace mail_system