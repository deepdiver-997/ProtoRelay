#pragma once


#include <mail_system/back/thread_pool/thread_pool_base.h>
#include <mail_system/back/thread_pool/io_thread_pool.h>
#include <mail_system/back/thread_pool/boost_thread_pool.h>

#include <mail_system/back/mailServer/session/session_base.h>

namespace mail_system {

void client_template_func(std::unique_ptr<SessionBase> session) {
    auto fsm = static_cast<SMTPSClientFSM*>(session->get_fsm());
    // 根据当前状态和服务器响应确定下一个事件
    auto client_session = static_cast<SMTPSClientSession*>(session.get());
    SMTPSClientEvent ev = SMTPSClientEvent::START; // 默认事件
    
    // 根据状态确定事件
    switch (client_session->state_) {
        case SMTPSClientState::SHAKED:
            ev = SMTPSClientEvent::START;
            break;
        case SMTPSClientState::AUTH:
            ev = SMTPSClientEvent::AUTH;
            break;
        case SMTPSClientState::READY:
            ev = SMTPSClientEvent::SET_MAIL_FROM;
            break;
        case SMTPSClientState::SENDING:
            ev = SMTPSClientEvent::SET_MAIL_TO;
            break;
        default:
            ev = SMTPSClientEvent::DONE;
            break;
    }
    
    std::string args = client_session->get_last_read_data(session->last_bytes_transferred_);
    fsm->process_event(std::move(session), ev);
}

enum class SMTPSClientState {
    INIT,
    SHAKED,
    AUTH,
    READY,
    SENDING,
    DONE
};

enum class SMTPSClientEvent {
    START,
    AUTH,
    SET_MAIL_FROM,
    SET_MAIL_TO,
    TRANSFER,
    DONE
};
class SMTPSClientFSM {
public:
    ~SMTPSClientFSM() = default;

    void start(std::unique_ptr<mail_system::SessionBase> session);

    explicit SMTPSClientFSM(std::shared_ptr<ThreadPoolBase> ioThreadPool = nullptr,
                          std::shared_ptr<ThreadPoolBase> workerThreadPool = nullptr,
                          std::shared_ptr<DBPool> dbPool = nullptr)
        : m_ioThreadPool(ioThreadPool), m_workerThreadPool(workerThreadPool), m_dbPool(dbPool) {}

    void process_event(std::unique_ptr<mail_system::SessionBase> session, SMTPSClientEvent event);
    
    void start_session(std::unique_ptr<SMTPSClientSession> session, std::queue<std::unique_ptr<mail>> mails);

    void start_session_internal(std::unique_ptr<SMTPSClientSession> session);

    private:
    std::shared_ptr<ThreadPoolBase> m_ioThreadPool;
    std::shared_ptr<ThreadPoolBase> m_workerThreadPool;
    std::shared_ptr<DBPool> m_dbPool;

    void auth(std::unique_ptr<mail_system::SessionBase> session, const std::string& username, const std::string& password);
    void set_mail_from(std::unique_ptr<mail_system::SessionBase> session);
    void set_mail_to(std::unique_ptr<mail_system::SessionBase> session);
    void transfer(std::unique_ptr<mail_system::SessionBase> session);
};

class SMTPSClientSession : public mail_system::SessionBase {
public:
    SMTPSClientState state_;
    std::queue<std::unique_ptr<mail>> emails_;
    std::unique_ptr<mail> email_;
    std::vector<std::string> recipients_;
    SMTPSClientFSM* m_fsm;
    int retry_count_ = 0;
    int sent_counts_ = 0;
    const int max_sent_ = 100;

    SMTPSClientSession(std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> ssl_socket,
                  ServerBase* server,
                  SMTPSClientFSM* fsm)
        : mail_system::SessionBase(std::move(ssl_socket), server), 
          state_(SMTPSClientState::INIT), 
          m_fsm(fsm) {}

    ~SMTPSClientSession() override = default;

    void handle_read() override {
        // 这个方法不应该被直接调用，而是通过client_template_func处理
        // 如果被调用，说明架构有问题
        std::cerr << "SMTPSClientSession::handle_read() should not be called directly" << std::endl;
    }

    void set_current_state(int state) override {
        state_ = static_cast<SMTPSClientState>(state);
    }

    void set_next_event(int event) override {
        next_event_ = static_cast<SMTPSClientEvent>(event);
    }

    void* get_fsm() const override {
        return m_fsm;
    }

    int get_next_event() const override {
        return static_cast<int>(next_event_);
    }

    int get_current_state() const override {
        return static_cast<int>(state_);
    }

    void* get_context() override {
        return &context_;
    }

    bool check() {
        return emails_.empty() | (sent_counts_ + emails_.front()->to.size() >= max_sent_);
    }

    void refresh() {
        sent_counts_ = 0;
        // quit and reconnect, then re-authenticate
        close();
        connect();
    }

    void connect() {
        // Implement connection logic here
        // after connected
        closed_ = false;
        state_ = SMTPSClientState::SHAKED;
    }

private:
    SMTPSClientEvent next_event_ = SMTPSClientEvent::START;
    
    struct SMTPSClientContext {
        std::string last_response;
        std::string auth_username;
        bool is_authenticated = false;
        void clear() {
            last_response.clear();
            auth_username.clear();
            is_authenticated = false;
        }
    } context_;

    // template <typename WriteHandler>
    // void async_write(const std::string& data, WriteHandler&& handler) {
    //     boost::asio::async_write(*m_socket, boost::asio::buffer(data), std::forward<WriteHandler>(handler));
    // }
};

void SMTPSClientFSM::start_session(std::unique_ptr<SMTPSClientSession> session, std::queue<std::unique_ptr<mail>> mails) {
    // 将邮件队列转移到会话中
    session->emails_ = std::move(mails);
    
    // 获取第一封邮件进行处理
    if (!session->emails_.empty()) {
        session->email_ = std::move(session->emails_.front());
        session->emails_.pop();
        session->recipients_ = session->email_->to;
        session->state_ = SMTPSClientState::INIT;
        
        // 开始发送流程
        start_session_internal(std::move(session));
    }
}

void SMTPSClientFSM::start_session_internal(std::unique_ptr<SMTPSClientSession> session) {
    auto session_base = std::unique_ptr<SessionBase>(static_cast<SessionBase*>(session.release()));
    
    SessionBase::do_handshake(std::move(session_base), [](std::unique_ptr<SessionBase> s, const boost::system::error_code &ec) {
        if (ec || s->is_closed()) {
            std::cerr << "SSL handshake failed or session closed: " << ec.message() << std::endl;
            return;
        }
        
        std::cout << "SSL handshake completed, sending EHLO" << std::endl;
        
        s->set_current_state(int(SMTPSClientState::SHAKED));
        SessionBase::async_write(std::move(s), "EHLO localhost\r\n", [](std::unique_ptr<SessionBase> ss, const boost::system::error_code &e){
            std::cout << "EHLO sent" << std::endl;
            if (e) {
                std::cerr << "Error sending EHLO: " << e.message() << std::endl;
                return;
            }
            if (ss && !ss->is_closed()) {
                std::cout << "EHLO sent successfully, starting auth" << std::endl;
                
                // 启动异步读取等待服务器响应
                SessionBase::async_read(std::move(ss), [](std::unique_ptr<SessionBase> sss, const boost::system::error_code& read_ec, std::size_t bytes_transferred){
                    if (read_ec || !sss || sss->is_closed()) {
                        std::cerr << "Error reading after EHLO: " << (read_ec ? read_ec.message() : "Session null/closed") << std::endl;
                        return;
                    }
                    
                    client_template_func(std::move(sss));
                });
            } else {
                std::cout << "Session is null or closed in callback" << std::endl;
            }
        });
    });
}

void SMTPSClientFSM::auth(std::unique_ptr<mail_system::SessionBase> session, const std::string& username, const std::string& password) {
    // Send AUTH command
    std::string auth_command = "AUTH LOGIN\r\n";
    SessionBase::async_write(std::move(session), auth_command, [this, username, password](std::unique_ptr<SessionBase> s, const boost::system::error_code& ec) {
        if (ec) {
            std::cerr << "Error sending AUTH command: " << ec.message() << std::endl;
            return;
        }
        if (!s || s->is_closed()) {
            std::cerr << "Session null or closed" << std::endl;
            return;
        }
        
        // Send username
        SessionBase::async_write(std::move(s), username + "\r\n", [this, password](std::unique_ptr<SessionBase> ss, const boost::system::error_code& ec) {
            if (ec) {
                std::cerr << "Error sending username: " << ec.message() << std::endl;
                return;
            }
            if (!ss || ss->is_closed()) {
                std::cerr << "Session null or closed" << std::endl;
                return;
            }
            
            // Send password
            SessionBase::async_write(std::move(ss), password + "\r\n", [this](std::unique_ptr<SessionBase> sss, const boost::system::error_code& ec) {
                if (ec) {
                    std::cerr << "Error sending password: " << ec.message() << std::endl;
                    return;
                }
                if (!sss || sss->is_closed()) {
                    std::cerr << "Session null or closed" << std::endl;
                    return;
                }
                
                // Authentication successful, wait for server response
                SessionBase::async_read(std::move(sss), [](std::unique_ptr<SessionBase> ssss, const boost::system::error_code& read_ec, std::size_t bytes_transferred){
                    if (read_ec || !ssss || ssss->is_closed()) {
                        std::cerr << "Error reading auth response: " << (read_ec ? read_ec.message() : "Session null/closed") << std::endl;
                        return;
                    }
                    
                    client_template_func(std::move(ssss));
                });
            });
        });
    });
}

void SMTPSClientFSM::set_mail_from(std::unique_ptr<mail_system::SessionBase> session) {
    auto client_session = static_cast<SMTPSClientSession*>(session.get());
    
    // check and refresh if necessary
    if (client_session->check()) {
        client_session->refresh();
    }
    
    // Send MAIL FROM command
    std::string mail_from_command = "MAIL FROM:<" + client_session->email_->from + ">\r\n";
    SessionBase::async_write(std::move(session), mail_from_command, [this](std::unique_ptr<SessionBase> s, const boost::system::error_code& ec) {
        if (ec) {
            std::cerr << "Error sending MAIL FROM: " << ec.message() << std::endl;
            return;
        }
        if (!s || s->is_closed()) {
            std::cerr << "Session null or closed" << std::endl;
            return;
        }
        
        // Wait for server response
        SessionBase::async_read(std::move(s), [](std::unique_ptr<SessionBase> ss, const boost::system::error_code& read_ec, std::size_t bytes_transferred){
            if (read_ec || !ss || ss->is_closed()) {
                std::cerr << "Error reading MAIL FROM response: " << (read_ec ? read_ec.message() : "Session null/closed") << std::endl;
                return;
            }
            
            client_template_func(std::move(ss));
        });
    });
}

void SMTPSClientFSM::set_mail_to(std::unique_ptr<mail_system::SessionBase> session) {
    auto client_session = static_cast<SMTPSClientSession*>(session.get());
    
    // Send RCPT TO command for each recipient
    std::string mail_to_command = "RCPT TO:<" + client_session->recipients_[0] + ">\r\n";
    SessionBase::async_write(std::move(session), mail_to_command, [this](std::unique_ptr<SessionBase> s, const boost::system::error_code& ec) {
        if (ec) {
            std::cerr << "Error sending RCPT TO: " << ec.message() << std::endl;
            return;
        }
        if (!s || s->is_closed()) {
            std::cerr << "Session null or closed" << std::endl;
            return;
        }
        
        // Wait for server response
        SessionBase::async_read(std::move(s), [](std::unique_ptr<SessionBase> ss, const boost::system::error_code& read_ec, std::size_t bytes_transferred){
            if (read_ec || !ss || ss->is_closed()) {
                std::cerr << "Error reading RCPT TO response: " << (read_ec ? read_ec.message() : "Session null/closed") << std::endl;
                return;
            }
            
            client_template_func(std::move(ss));
        });
    });
}

void SMTPSClientFSM::transfer(std::unique_ptr<mail_system::SessionBase> session) {
    auto client_session = static_cast<SMTPSClientSession*>(session.get());
    
    // Send DATA command
    std::string data_command = "DATA\r\n";
    SessionBase::async_write(std::move(session), data_command, [this](std::unique_ptr<SessionBase> s, const boost::system::error_code& ec) {
        if (ec) {
            std::cerr << "Error sending DATA: " << ec.message() << std::endl;
            return;
        }
        if (!s || s->is_closed()) {
            std::cerr << "Session null or closed" << std::endl;
            return;
        }
        
        // Wait for server response (354)
        SessionBase::async_read(std::move(s), [this](std::unique_ptr<SessionBase> ss, const boost::system::error_code& read_ec, std::size_t bytes_transferred){
            if (read_ec || !ss || ss->is_closed()) {
                std::cerr << "Error reading DATA response: " << (read_ec ? read_ec.message() : "Session null/closed") << std::endl;
                return;
            }
            
            auto client_session = static_cast<SMTPSClientSession*>(ss.get());
            
            // Send email header
            std::string header = client_session->email_->header + "\r\n";
                SessionBase::async_write(std::move(ss), header, [this](std::unique_ptr<SessionBase> sss, const boost::system::error_code& ec) {
                    if (ec) {
                        std::cerr << "Error sending header: " << ec.message() << std::endl;
                        return;
                    }
                    if (!sss || sss->is_closed()) {
                        std::cerr << "Session null or closed" << std::endl;
                        return;
                    }
                    
                    auto client_session = static_cast<SMTPSClientSession*>(sss.get());
                    
                    // Send email body
                    SessionBase::async_write(std::move(sss), client_session->email_->body, [this](std::unique_ptr<SessionBase> ssss, const boost::system::error_code& ec) {
                    if (ec) {
                        std::cerr << "Error sending body: " << ec.message() << std::endl;
                        return;
                    }
                    if (!ssss || ssss->is_closed()) {
                        std::cerr << "Session null or closed" << std::endl;
                        return;
                    }
                    
                    // End of email
                    SessionBase::async_write(std::move(ssss), "\r\n.\r\n", [this](std::unique_ptr<SessionBase> sssss, const boost::system::error_code& ec) {
                        if (ec) {
                            std::cerr << "Error sending end marker: " << ec.message() << std::endl;
                            return;
                        }
                        if (!sssss || sssss->is_closed()) {
                            std::cerr << "Session null or closed" << std::endl;
                            return;
                        }
                        
                        // Wait for server response (250)
                        SessionBase::async_read(std::move(sssss), [](std::unique_ptr<SessionBase> ssssss, const boost::system::error_code& read_ec, std::size_t bytes_transferred){
                            if (read_ec || !ssssss || ssssss->is_closed()) {
                                std::cerr << "Error reading final response: " << (read_ec ? read_ec.message() : "Session null/closed") << std::endl;
                                return;
                            }
                            
                            std::cout << "Email sent successfully" << std::endl;
                            
                            // Send QUIT to close connection
                            SessionBase::async_write(std::move(ssssss), "QUIT\r\n", [](std::unique_ptr<SessionBase> final_session, const boost::system::error_code& ec) {
                                if (ec) {
                                    std::cerr << "Error sending QUIT: " << ec.message() << std::endl;
                                }
                                if (final_session) {
                                    final_session->close();
                                }
                            });
                        });
                    });
                });
            });
        });
    });
}

void SMTPSClientFSM::process_event(std::unique_ptr<mail_system::SessionBase> session, SMTPSClientEvent event) {
    auto client_session = static_cast<SMTPSClientSession*>(session.get());
    
    std::cout << "Client FSM: Current State: " << static_cast<int>(client_session->state_) 
              << ", Event: " << static_cast<int>(event) << std::endl;
    
    switch (client_session->state_) {
        case SMTPSClientState::SHAKED:
            if (event == SMTPSClientEvent::START) {
                client_session->state_ = SMTPSClientState::AUTH;
                auth(std::move(session), "username", "password");
            }
            break;
            
        case SMTPSClientState::AUTH:
            if (event == SMTPSClientEvent::AUTH) {
                client_session->state_ = SMTPSClientState::READY;
                set_mail_from(std::move(session));
            }
            break;
            
        case SMTPSClientState::READY:
            if (event == SMTPSClientEvent::SET_MAIL_FROM) {
                client_session->state_ = SMTPSClientState::SENDING;
                set_mail_to(std::move(session));
            }
            break;
            
        case SMTPSClientState::SENDING:
            if (event == SMTPSClientEvent::SET_MAIL_TO) {
                transfer(std::move(session));
            }
            break;
            
        default:
            std::cerr << "Invalid state transition" << std::endl;
            break;
    }
}

void SMTPSClientFSM::start(std::unique_ptr<mail_system::SessionBase> session) {
    // For backward compatibility, just call start_session_internal
    start_session_internal(std::unique_ptr<SMTPSClientSession>(static_cast<SMTPSClientSession*>(session.release())));
}

} // namespace mail_system