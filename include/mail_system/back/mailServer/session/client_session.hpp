#pragma once

#include <mail_system/back/mailServer/fsm/client/client_fsm.hpp>
#include <mail_system/back/mailServer/session/session_base.h>

namespace mail_system {

enum class ClientState {
    INIT,
    SHAKED,
    AUTH,
    READY,
    SENDING,
    DONE
};

enum class ClientEvent {
    START,
    AUTH,
    SET_MAIL_FROM,
    SET_MAIL_TO,
    TRANSFER,
    DONE
};
class ClientFSM;
class ClientSession : public SessionBase {
public:
    ClientState state_;
    std::shared_ptr<mail> email_;
    std::shared_ptr<ClientFSM> m_fsm;
    std::vector<std::string> recipients_;
    int retry_count_ = 0;

    ClientSession(std::shared_ptr<mail> email,
                  std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> ssl_socket,
                  ServerBase* server,
                  std::shared_ptr<ClientFSM> fsm,
                  const std::vector<std::string>& recipients)
        : SessionBase(std::move(ssl_socket), server), state_(ClientState::INIT), email_(email), recipients_(recipients), m_fsm(fsm) {}

    ~ClientSession() override = default;

    void start() override {
            std::cout << "SMTPS Session started" << std::endl;
        if(m_socket == nullptr) {
            std::cerr << "SMTPS Session socket is null in start." << std::endl;
            return; // 确保socket已初始化
        }
        if(!m_socket->lowest_layer().is_open()) {
            std::cerr << "SMTPS Session socket is not open in start." << std::endl;
            return; // 确保socket已打开
        }
        if(closed_) {
            std::cout << "Session already closed in SmtpsSession::start." << std::endl;
            return; // 已经关闭
        }
        std::cout << "ready to call process event\n";
        m_fsm->start(std::dynamic_pointer_cast<ClientSession>(shared_from_this()));
        std::cout << "start event CONNECT called in SmtpsSession::start" << std::endl;
    }

    void handle_read(const std::string& data) override {
        // Handle the read data
        // auto self = shared_from_this();
        // std::string line = data;
        // boost::algorithm::trim_right_if(line, boost::algorithm::is_any_of("\r\n"));
    }

    // template <typename WriteHandler>
    // void async_write(const std::string& data, WriteHandler&& handler) {
    //     boost::asio::async_write(*m_socket, boost::asio::buffer(data), std::forward<WriteHandler>(handler));
    // }
};

} // namespace mail_system