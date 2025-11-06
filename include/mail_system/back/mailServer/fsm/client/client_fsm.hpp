#pragma once

// #include <mail_system/back/mailServer/session/client_session.hpp>

#include <mail_system/back/thread_pool/thread_pool_base.h>
#include <mail_system/back/thread_pool/io_thread_pool.h>
#include <mail_system/back/thread_pool/boost_thread_pool.h>

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
class ClientFSM {
public:
    ~ClientFSM() = default;

    void start(std::shared_ptr<mail_system::SessionBase> session);

    explicit ClientFSM() {}

    void process_event(std::shared_ptr<mail_system::SessionBase> session, ClientEvent event) {};

    private:

    void auth(std::shared_ptr<mail_system::SessionBase> session, const std::string& username, const std::string& password);
    void set_mail_from(std::shared_ptr<mail_system::SessionBase> session);
    void set_mail_to(std::shared_ptr<mail_system::SessionBase> session);
    void transfer(std::shared_ptr<mail_system::SessionBase> session);
};

class ClientSession : public mail_system::SessionBase {
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
        : mail_system::SessionBase(std::move(ssl_socket), server), state_(ClientState::INIT), email_(email), recipients_(recipients), m_fsm(fsm) {}

    ~ClientSession() override = default;

    void start() override {
        // std::cout << "SMTPS Session started" << std::endl;
        // if(m_socket == nullptr) {
        //     std::cerr << "SMTPS Session socket is null in start." << std::endl;
        //     return; // 确保socket已初始化
        // }
        // if(!m_socket->lowest_layer().is_open()) {
        //     std::cerr << "SMTPS Session socket is not open in start." << std::endl;
        //     return; // 确保socket已打开
        // }
        // if(closed_) {
        //     std::cout << "Session already closed in SmtpsSession::start." << std::endl;
        //     return; // 已经关闭
        // }
        // std::cout << "ready to call process event\n";
        auto self = shared_from_this();
        m_fsm->start(self);
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

void ClientFSM::start(std::shared_ptr<mail_system::SessionBase> session) {
    session->do_handshake([this, session](std::weak_ptr<mail_system::SessionBase> s, const boost::system::error_code& error) {
        auto session = s.lock();
        if (!session) {
            std::cerr << "Session expired during handshake" << std::endl;
            return;
        }
        // Handshake successful, send EHLO command
        std::string ehlo_command = "EHLO localhost\r\n";
        session->async_write(ehlo_command, [session, this] (const boost::system::error_code& ec) {
            if (!ec) {
                this->auth(std::dynamic_pointer_cast<mail_system::SessionBase>(session), "username", "password");
            }
        });
    });
}

void ClientFSM::auth(std::shared_ptr<mail_system::SessionBase> session, const std::string& username, const std::string& password) {
    // Send AUTH command
    std::string auth_command = "AUTH LOGIN\r\n";
    session->async_write(auth_command, [session, this, username, password](const boost::system::error_code& ec) {
        if (!ec) {
            // Send username
            session->async_write(username + "\r\n", [session, this, password](const boost::system::error_code& ec) {
                if (!ec) {
                    // Send password
                    session->async_write(password + "\r\n", [session, this](const boost::system::error_code& ec) {
                        if (!ec) {
                            // Authentication successful
                            this->set_mail_from(session);
                        }
                    });
                }
            });
        }
    });
}

void ClientFSM::set_mail_from(std::shared_ptr<mail_system::SessionBase> session) {
    // Send MAIL FROM command
    std::string mail_from_command = "MAIL FROM:<" + std::dynamic_pointer_cast<ClientSession>(session)->email_->from + ">\r\n";
    session->async_write(mail_from_command, [session, this](const boost::system::error_code& ec) {
        if (!ec) {
            this->set_mail_to(session);
        }
    });
}

void ClientFSM::set_mail_to(std::shared_ptr<mail_system::SessionBase> session) {
    // Send MAIL TO command
    std::string mail_to_command = "RCPT TO:<";
    for (const auto& addr : std::dynamic_pointer_cast<ClientSession>(session)->recipients_) {
        mail_to_command += addr + ",";
    }
    mail_to_command.back() = '>';  // Replace last comma with closing bracket
    mail_to_command += "\r\n";
    session->async_write(mail_to_command, [session, this](const boost::system::error_code& ec) {
        if (!ec) {
            this->transfer(session);
        }
    });
}

void ClientFSM::transfer(std::shared_ptr<mail_system::SessionBase> session) {
    // Send DATA command
    std::string data_command = "DATA\r\n";
    session->async_write(data_command, [session, this](const boost::system::error_code& ec) {
        if (!ec) {
            session->async_write(std::dynamic_pointer_cast<ClientSession>(session)->email_->header, [session, this](const boost::system::error_code& ec) {
                if (!ec) {
                    // Send email body
                    session->async_write(std::dynamic_pointer_cast<ClientSession>(session)->email_->body, [session, this](const boost::system::error_code& ec) {
                        if (!ec) {
                            // End of email
                            session->async_write("\r\n.\r\n", [session, this](const boost::system::error_code& ec) {
                                if (!ec) {
                                    // Email sent successfully
                                    session->async_write("QUIT\r\n", [session, this](const boost::system::error_code& ec) {
                                        if (!ec) {
                                            // QUIT command sent successfully
                                        }
                                    });
                                }
                            });
                        }
                    });
                }
            });
        }
    });
}

} // namespace mail_system