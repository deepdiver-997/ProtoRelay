#include "mail_system/back/mailServer/session/smtps_session.h"
#include "mail_system/back/mailServer/fsm/smtps/smtps_fsm.h"
#include <iostream>
#include <boost/algorithm/string.hpp>

namespace mail_system {

SmtpsSession::SmtpsSession(ServerBase* server, std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> &&socket, std::shared_ptr<SmtpsFsm> fsm)
    : SessionBase(std::move(socket), server), current_state_(SmtpsState::INIT), m_fsm(fsm), m_receivingData(false) {
    if (!m_fsm) {
        throw std::invalid_argument("SmtpsSession: FSM cannot be null");
    }
}

SmtpsSession::~SmtpsSession() {
    // 确保会话关闭
    close();
}

void SmtpsSession::start(std::unique_ptr<SmtpsSession> self) {
    // std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    std::cout << "SMTPS Session started" << std::endl;
    if(self->m_socket == nullptr) {
        std::cerr << "SMTPS Session socket is null in start." << std::endl;
        return; // 确保socket已初始化
    }
    if(!self->m_socket->lowest_layer().is_open()) {
        std::cerr << "SMTPS Session socket is not open in start." << std::endl;
        return; // 确保socket已打开
    }
    if(self->closed_) {
        std::cout << "Session already closed in SmtpsSession::start." << std::endl;
        return; // 已经关闭
    }
    std::cout << "ready to call process event\n";
    self->next_event_ = SmtpsEvent::CONNECT;
    auto fsm = self->m_fsm;
    fsm->process_event(std::move(self), SmtpsEvent::CONNECT, std::string());
    std::cout << "start event CONNECT called in SmtpsSession::start" << std::endl;
    return;
}

void SmtpsSession::handle_read() {
    try {
        // 对象有效性检查
        if (this == nullptr) {
            std::cerr << "CRITICAL: Invalid this pointer in handle_read" << std::endl;
            return;
        }
        if (!m_fsm) {
            std::cerr << "CRITICAL: m_fsm is null in handle_read" << std::endl;
            return;
        }
        std::cout << "enter handle_read in SmtpsSession" << std::endl;
        // 使用最近一次实际读取的字节数构造数据，避免读取到填充的\0
        std::string data = get_last_read_data(last_bytes_transferred_);
        
        // 检查空数据，如果是空数据则忽略并继续等待
        if (data.empty() || (data.find_first_not_of("\r\n") == std::string::npos)) {
            std::cout << "Ignoring empty data, continuing to wait for commands" << std::endl;
            return; // 不设置任何事件，继续等待实际数据
        }
        
        // 去除行首/行尾空白与行尾的 CRLF
        std::string line = data;
        boost::algorithm::trim(line);
        
        if (current_state_ == SmtpsState::IN_MESSAGE) {
            // 检查是否为数据结束标记（单行只有一个点）
            if (line == ".") {
                // 处理邮件数据结束事件
                next_event_ = SmtpsEvent::DATA_END;
                return;
            }
            else {
                // 如果行以.开头，去掉一个.（SMTP协议规定）
                if (!line.empty() && line[0] == '.') {
                    line = line.substr(1);
                }
                if (mail_ == nullptr) {
                    mail_ = std::make_unique<mail>();
                }
                mail_->header = line.substr(0, line.find("\n\n"));
                mail_->body = line.substr(line.find("\n\n") + 2);
                
                // 处理数据事件（可选，取决于状态机是否需要处理每一行数据）
                // // 调试信息
                // std::cout << "[DEBUG] Object address: " << this << std::endl;
                // std::cout << "[DEBUG] FSM vtable: " << typeid(*m_fsm).name() << std::endl;
                // // 调用栈回溯
                // std::cout << "[DEBUG] Call stack trace: " << std::endl;
                next_event_ = SmtpsEvent::DATA;
            }
        }
        else {
            // 处理命令
            process_command(line);
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Error handling SMTPS data: " << e.what() << std::endl;
        next_event_ = SmtpsEvent::ERROR;
    }
}

void SmtpsSession::process_command(const std::string& command) {
    try {
        std::cout << "SMTPS command: " << command << std::endl;
        
        // 提取命令和参数
        std::string cmd;
        std::string args;
        
        size_t space_pos = command.find(' ');
        if (space_pos != std::string::npos) {
            cmd = command.substr(0, space_pos);
            args = command.substr(space_pos + 1);
        }
        else {
            cmd = command;
            args = "";
        }

        // 保存参数供FSM使用
        last_command_args_ = args;
        
        std::cout << "DEBUG: cmd='" << cmd << "', args='" << args << "'" << std::endl;

        // 检查空命令，如果是空命令则不处理
        if (cmd.empty()) {
            std::cout << "Ignoring empty command, not setting any event" << std::endl;
            return;
        }

        if(current_state_ == SmtpsState::WAIT_AUTH_USERNAME || current_state_ == SmtpsState::WAIT_AUTH_PASSWORD) {
            next_event_ = SmtpsEvent::AUTH;
            return;
        }
        
        // 转换命令为大写
        boost::algorithm::to_upper(cmd);
        
        // 将命令映射到事件
        if (cmd == "EHLO" || cmd == "HELO") {
            next_event_ = SmtpsEvent::EHLO;
        }
        else if (cmd == "AUTH") {
            next_event_ = SmtpsEvent::AUTH;
        } else if (cmd == "MAIL") {
            next_event_ = SmtpsEvent::MAIL_FROM;
        } else if (cmd == "RCPT") {
            next_event_ = SmtpsEvent::RCPT_TO;
        } else if (cmd == "DATA") {
            next_event_ = SmtpsEvent::DATA;
        } else if (cmd == "QUIT") {
            next_event_ = SmtpsEvent::QUIT;
        } else {
            // 未知命令
            next_event_ = SmtpsEvent::ERROR;
            args = "Unknown command: " + cmd;
            std::cout << args << std::endl;
        }
        std::cout << "DEBUG: event=" << m_fsm->get_event_name(next_event_) << std::endl;
    }
    catch (const std::exception& e) {
        std::cerr << "Error processing SMTPS command: " << e.what() << std::endl;
        // 处理错误事件
    }
}

} // namespace mail_system