#include "mail_system/back/mailServer/session/imaps_session.h"
#include "mail_system/back/mailServer/fsm/imaps/imaps_fsm.h"
#include <iostream>
#include <boost/algorithm/string.hpp>

namespace mail_system {

ImapsSession::ImapsSession(ServerBase* server, std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> &&socket, std::shared_ptr<ImapsFsm> fsm)
    : SessionBase(std::move(socket), server), current_state_(ImapsState::INIT), m_fsm(fsm), m_receivingData(false), stay_times(0) {
    if (!m_fsm) {
        throw std::invalid_argument("ImapsSession: FSM cannot be null");
    }
}

ImapsSession::~ImapsSession() {
    // 确保会话关闭
    close();
}

void ImapsSession::start() {
    std::cout << "IMAPS Session started" << std::endl;
    if(m_socket == nullptr) {
        std::cerr << "IMAPS Session socket is null in start." << std::endl;
        return; // 确保socket已初始化
    }
    if(!m_socket->lowest_layer().is_open()) {
        std::cerr << "IMAPS Session socket is not open in start." << std::endl;
        return; // 确保socket已打开
    }
    if(closed_) {
        std::cout << "Session already closed in ImapsSession::start." << std::endl;
        return; // 已经关闭
    }
    std::cout << "ready to call process event\n";
    m_fsm->process_event(std::dynamic_pointer_cast<ImapsSession>(shared_from_this()), ImapsEvent::CONNECT, std::vector<std::string>());
    std::cout << "start event CONNECT called in ImapsSession::start" << std::endl;
}

void ImapsSession::handle_read(const std::string& data) {
    try {
        // 对象有效性检查
        if (this == nullptr) {
            std::cerr << "CRITICAL: Invalid this pointer in handle_read" << std::endl;
            return;
        }
        try {
            auto self = shared_from_this(); // 验证shared_ptr有效性
        } catch (const std::bad_weak_ptr& e) {
            std::cerr << "CRITICAL: Invalid shared_from_this(): " << e.what() << std::endl;
            return;
        }
        if (!m_fsm) {
            std::cerr << "CRITICAL: m_fsm is null in handle_read" << std::endl;
            return;
        }
        std::cout << "enter handle_read in ImapsSession" << std::endl;
        auto self = shared_from_this();
        // 去除行尾的\r\n
        std::string line = data;
        boost::algorithm::trim_right_if(line, boost::algorithm::is_any_of("\r\n"));
        
        if (current_state_ == ImapsState::WAIT_FETCH || current_state_ == ImapsState::WAIT_STORE) {
            // 处理FETCH或STORE命令的数据
            m_fsm->process_event(std::dynamic_pointer_cast<ImapsSession>(self), ImapsEvent::FETCH, std::vector<std::string>(1, line));
        } else {
            // 处理普通命令
            process_command(line);
        }
    } catch (const std::exception& e) {
        std::cerr << "Exception in ImapsSession::handle_read: " << e.what() << std::endl;
    }
}

void ImapsSession::process_command(const std::string& command) {
    try {
        auto self = shared_from_this();
        std::cout << "Processing IMAP command: " << command << std::endl;
        
        // 解析命令
        std::vector<std::string> tokens;
        boost::algorithm::split(tokens, command, boost::algorithm::is_any_of(" "), boost::algorithm::token_compress_on);
        
        if (tokens.empty()) {
            std::cerr << "Empty command received" << std::endl;
            return;
        }

        context_.cmd_tag = tokens[0];

        std::string cmd = boost::algorithm::to_upper_copy(tokens[1]);
        
        if (cmd == "LOGIN") {
            m_fsm->process_event(std::dynamic_pointer_cast<ImapsSession>(self), ImapsEvent::LOGIN, std::vector<std::string>(tokens.begin() + 2, tokens.end()));
        } else if (cmd == "SELECT") {
            m_fsm->process_event(std::dynamic_pointer_cast<ImapsSession>(self), ImapsEvent::SELECT, std::vector<std::string>(tokens.begin() + 2, tokens.end()));
        } else if (cmd == "FETCH") {
            m_fsm->process_event(std::dynamic_pointer_cast<ImapsSession>(self), ImapsEvent::FETCH, std::vector<std::string>(tokens.begin() + 2, tokens.end()));
        } else if (cmd == "STORE") {
            m_fsm->process_event(std::dynamic_pointer_cast<ImapsSession>(self), ImapsEvent::STORE, std::vector<std::string>(tokens.begin() + 2, tokens.end()));
        } else if (cmd == "EXPUNGE") {
            m_fsm->process_event(std::dynamic_pointer_cast<ImapsSession>(self), ImapsEvent::EXPUNGE, std::vector<std::string>(tokens.begin() + 2, tokens.end()));
        } else if (cmd == "CLOSE") {
            m_fsm->process_event(std::dynamic_pointer_cast<ImapsSession>(self), ImapsEvent::CLOSE, std::vector<std::string>(tokens.begin() + 2, tokens.end()));
        } else if (cmd == "LOGOUT") {
            m_fsm->process_event(std::dynamic_pointer_cast<ImapsSession>(self), ImapsEvent::LOGOUT, std::vector<std::string>(tokens.begin() + 2, tokens.end()));
        } else {
            std::cerr << "Unknown IMAP command: " << cmd << std::endl;
            async_write("BAD Unknown command\r\n");
        }
    } catch (const std::exception& e) {
        std::cerr << "Exception in ImapsSession::process_command: " << e.what() << std::endl;
    }
}

} // namespace mail_system