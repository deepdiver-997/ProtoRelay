#include "mail_system/back/mailServer/fsm/smtps/traditional_smtps_fsm.h"
#include "mail_system/back/mailServer/session/smtps_session.h"
#include <memory>
#include <iostream>

namespace mail_system {

void template_func(std::unique_ptr<SessionBase> session) {
    session->handle_read();
    auto fsm = static_cast<TraditionalSmtpsFsm*>(session->get_fsm());
    SmtpsEvent ev = static_cast<SmtpsEvent>(session->get_next_event());
    std::string args = static_cast<SmtpsSession*>(session.get())->get_last_command_args();
    fsm->process_event(std::move(session), ev, args);
}

TraditionalSmtpsFsm::TraditionalSmtpsFsm(std::shared_ptr<ThreadPoolBase> io_thread_pool,
                                           std::shared_ptr<ThreadPoolBase> worker_thread_pool,
                                           std::shared_ptr<DBPool> db_pool)
    : SmtpsFsm(io_thread_pool, worker_thread_pool, db_pool) {
    init_transition_table();
    init_state_handlers();
}

void TraditionalSmtpsFsm::process_event(std::unique_ptr<SessionBase> session, SmtpsEvent event, const std::string& args) {
    std::cout << "enter process_event\n";
    std::cout << "Current State: " << get_state_name(static_cast<SmtpsState>(session->get_current_state())) 
              << ", Event: " << get_event_name(event) << std::endl;
    if(static_cast<SmtpsState>(session->get_current_state()) == SmtpsState::CLOSED) {
        session->close();
        return;
    }

    if(static_cast<SmtpsState>(session->get_current_state()) == SmtpsState::IN_MESSAGE && event == SmtpsEvent::DATA)
        std::cout << "in message with data\n";
    auto transition_key = std::make_pair(static_cast<SmtpsState>(session->get_current_state()), event);
    auto transition_it = transition_table_.find(transition_key);
    
    if (transition_it != transition_table_.end()) {
        // 找到有效的状态转换
        SmtpsState next_state = transition_it->second;
        
        // 查找状态处理函数
        auto state_handler_it = state_handlers_.find(static_cast<SmtpsState>(session->get_current_state()));
        if (state_handler_it != state_handlers_.end()) {
            auto event_handler_it = state_handler_it->second.find(event);
            if (event_handler_it != state_handler_it->second.end()) {
                // 执行状态处理函数 - 直接在当前线程调用以避免unique_ptr复制问题
                auto handler = event_handler_it->second;
                // std::cout << "before handler\n";
                handler(std::move(session), args);
                // 更新会话状态
                // session->set_current_state(next_state);
                return;
            }
        }
        else {
            std::cout << "No handler for state " << get_state_name(static_cast<SmtpsState>(session->get_current_state())) << " and event " << get_event_name(event) << std::endl;
            return;
        }
        
        std::cout << "SMTPS FSM: " << get_state_name(static_cast<SmtpsState>(session->get_current_state())) << " -> " 
                  << get_event_name(event) << " -> " << get_state_name(next_state) << std::endl;
    } else {
        // 无效的状态转换
        std::cerr << "SMTPS FSM: Invalid transition from " << get_state_name(static_cast<SmtpsState>(session->get_current_state())) 
                  << " on event " << get_event_name(event) << std::endl;
        
        // 处理错误
        handle_error(std::move(session), "Invalid command sequence");
    }
}

void TraditionalSmtpsFsm::init_transition_table() {
    // 初始化状态转换表
    transition_table_[std::make_pair(SmtpsState::INIT, SmtpsEvent::CONNECT)] = SmtpsState::GREETING;
    transition_table_[std::make_pair(SmtpsState::WAIT_EHLO, SmtpsEvent::EHLO)] = SmtpsState::WAIT_AUTH;
    transition_table_[std::make_pair(SmtpsState::GREETING, SmtpsEvent::EHLO)] = SmtpsState::WAIT_AUTH;
    transition_table_[std::make_pair(SmtpsState::WAIT_AUTH, SmtpsEvent::AUTH)] = SmtpsState::WAIT_AUTH_USERNAME;
    transition_table_[std::make_pair(SmtpsState::WAIT_AUTH_USERNAME, SmtpsEvent::AUTH)] = SmtpsState::WAIT_AUTH_PASSWORD;
    transition_table_[std::make_pair(SmtpsState::WAIT_AUTH_PASSWORD, SmtpsEvent::AUTH)] = SmtpsState::WAIT_MAIL_FROM;
    // 添加可选认证路径 - 允许直接从WAIT_AUTH状态转到WAIT_MAIL_FROM状态
    transition_table_[std::make_pair(SmtpsState::WAIT_AUTH, SmtpsEvent::MAIL_FROM)] = SmtpsState::WAIT_RCPT_TO;

    transition_table_[std::make_pair(SmtpsState::WAIT_MAIL_FROM, SmtpsEvent::MAIL_FROM)] = SmtpsState::WAIT_RCPT_TO;
    transition_table_[std::make_pair(SmtpsState::WAIT_RCPT_TO, SmtpsEvent::RCPT_TO)] = SmtpsState::WAIT_DATA;
    transition_table_[std::make_pair(SmtpsState::WAIT_DATA, SmtpsEvent::DATA)] = SmtpsState::IN_MESSAGE;
    transition_table_[std::make_pair(SmtpsState::IN_MESSAGE, SmtpsEvent::DATA)] = SmtpsState::IN_MESSAGE;
    transition_table_[std::make_pair(SmtpsState::IN_MESSAGE, SmtpsEvent::DATA_END)] = SmtpsState::WAIT_QUIT;
    // 继续发送下一封邮件
    transition_table_[std::make_pair(SmtpsState::WAIT_QUIT, SmtpsEvent::MAIL_FROM)] = SmtpsState::WAIT_RCPT_TO;
    
    // QUIT命令可以在多个状态下接收
    for (int i = 0;i < 11; ++i)
        transition_table_[std::make_pair(static_cast<SmtpsState>(i), SmtpsEvent::QUIT)] = SmtpsState::CLOSED;
    
    for (int i = 0;i < 11; ++i) {
        // 错误处理
        transition_table_[std::make_pair(static_cast<SmtpsState>(i), SmtpsEvent::ERROR)] = static_cast<SmtpsState>(i);
        // 超时处理
        transition_table_[std::make_pair(static_cast<SmtpsState>(i), SmtpsEvent::TIMEOUT)] = static_cast<SmtpsState>(i);
    }
}

void TraditionalSmtpsFsm::init_state_handlers() {
    // 初始化状态处理函数
    state_handlers_[SmtpsState::INIT][SmtpsEvent::CONNECT] = 
        std::bind(&TraditionalSmtpsFsm::handle_init_connect, this, std::placeholders::_1, std::placeholders::_2);

    state_handlers_[SmtpsState::WAIT_EHLO][SmtpsEvent::EHLO] = 
        std::bind(&TraditionalSmtpsFsm::handle_greeting_ehlo, this, std::placeholders::_1, std::placeholders::_2);
    
    state_handlers_[SmtpsState::GREETING][SmtpsEvent::EHLO] = 
        std::bind(&TraditionalSmtpsFsm::handle_greeting_ehlo, this, std::placeholders::_1, std::placeholders::_2);

    state_handlers_[SmtpsState::WAIT_AUTH][SmtpsEvent::AUTH] = 
        std::bind(&TraditionalSmtpsFsm::handle_wait_auth_auth, this, std::placeholders::_1, std::placeholders::_2);

    state_handlers_[SmtpsState::WAIT_AUTH_USERNAME][SmtpsEvent::AUTH] = 
        std::bind(&TraditionalSmtpsFsm::handle_wait_auth_username, this, std::placeholders::_1, std::placeholders::_2);

    state_handlers_[SmtpsState::WAIT_AUTH_PASSWORD][SmtpsEvent::AUTH] = 
        std::bind(&TraditionalSmtpsFsm::handle_wait_auth_password, this, std::placeholders::_1, std::placeholders::_2);
    
    // 添加可选认证路径的处理函数
    state_handlers_[SmtpsState::WAIT_AUTH][SmtpsEvent::MAIL_FROM] = 
        std::bind(&TraditionalSmtpsFsm::handle_wait_auth_mail_from, this, std::placeholders::_1, std::placeholders::_2);
    
    state_handlers_[SmtpsState::WAIT_MAIL_FROM][SmtpsEvent::MAIL_FROM] = 
        std::bind(&TraditionalSmtpsFsm::handle_wait_mail_from_mail_from, this, std::placeholders::_1, std::placeholders::_2);
    
    state_handlers_[SmtpsState::WAIT_RCPT_TO][SmtpsEvent::RCPT_TO] = 
        std::bind(&TraditionalSmtpsFsm::handle_wait_rcpt_to_rcpt_to, this, std::placeholders::_1, std::placeholders::_2);
    
    state_handlers_[SmtpsState::WAIT_DATA][SmtpsEvent::DATA] = 
        std::bind(&TraditionalSmtpsFsm::handle_wait_data_data, this, std::placeholders::_1, std::placeholders::_2);

    state_handlers_[SmtpsState::IN_MESSAGE][SmtpsEvent::DATA] = 
        std::bind(&TraditionalSmtpsFsm::handle_in_message_data, this, std::placeholders::_1, std::placeholders::_2);
    
    state_handlers_[SmtpsState::IN_MESSAGE][SmtpsEvent::DATA_END] = 
        std::bind(&TraditionalSmtpsFsm::handle_in_message_data_end, this, std::placeholders::_1, std::placeholders::_2);
    
    // 退出处理函数
    for (int i = 1; i < static_cast<int>(SmtpsState::WAIT_QUIT) + 1; ++i) {
        state_handlers_[static_cast<SmtpsState>(i)][SmtpsEvent::QUIT] = 
            std::bind(&TraditionalSmtpsFsm::handle_wait_quit_quit, this, std::placeholders::_1, std::placeholders::_2);
    }

    // 继续发送下一封邮件处理函数
    state_handlers_[SmtpsState::WAIT_QUIT][SmtpsEvent::MAIL_FROM] = 
        std::bind(&TraditionalSmtpsFsm::handle_wait_auth_mail_from, this, std::placeholders::_1, std::placeholders::_2);
    
    // 错误处理函数
    for (int i = 0; i < static_cast<int>(SmtpsState::WAIT_QUIT) + 1; ++i) {
        state_handlers_[static_cast<SmtpsState>(i)][SmtpsEvent::ERROR] = 
            std::bind(&TraditionalSmtpsFsm::handle_error, this, std::placeholders::_1, std::placeholders::_2);
    }
}

void TraditionalSmtpsFsm::handle_init_connect(std::unique_ptr<SessionBase> session, const std::string& args) {
    std::cout << "handle_init_connect calling" << std::endl;
    
    // 直接转换到基类进行握手操作
    auto session_base = std::unique_ptr<SessionBase>(static_cast<SessionBase*>(session.release()));
    
    SessionBase::do_handshake(std::move(session_base), [](std::unique_ptr<SessionBase> s, const boost::system::error_code &ec) {
        if (ec || s->is_closed()) {
            std::cerr << "SSL handshake failed or session closed: " << ec.message() << std::endl;
            return;
        }
        
        std::cout << "SSL handshake completed, sending greeting" << std::endl;
        
        s->set_current_state(int(SmtpsState::GREETING));
        SessionBase::async_write(std::move(s), "220 SMTPS Server\r\n", [](std::unique_ptr<SessionBase> ss, const boost::system::error_code &e){
            std::cout << "async_write callback called" << std::endl;
            if (e) {
                std::cerr << "Error sending greeting: " << e.message() << std::endl;
                return;
            }
            if (ss && !ss->is_closed()) {
                std::cout << "Greeting sent successfully" << std::endl;
                ss->set_current_state(int(SmtpsState::WAIT_EHLO));
                std::cout << "State set to WAIT_EHLO, starting async_read" << std::endl;
                
                // 启动异步读取等待客户端命令
                SessionBase::async_read(std::move(ss), [](std::unique_ptr<SessionBase> sss, const boost::system::error_code& read_ec, std::size_t bytes_transferred){
                    if (read_ec) {
                        std::cerr << "Error reading in greeting callback: " << read_ec.message() << std::endl;
                        return;
                    }
                    if (!sss || sss->is_closed()) {
                        std::cerr << "Session null or closed in greeting read callback" << std::endl;
                        return;
                    }
                    
                    template_func(std::move(sss));
                });
            } else {
                std::cout << "Session is null or closed in callback" << std::endl;
            }
        });
    });
}

void TraditionalSmtpsFsm::handle_greeting_ehlo(std::unique_ptr<SessionBase> session, const std::string& args) {
    std::cout << "DEBUG: handle_greeting_ehlo called with args='" << args << "'" << std::endl;
    
    // 处理EHLO命令
    if (args.empty()) {
        SessionBase::async_write(std::move(session), "501 Syntax error in parameters or arguments\r\n", 
            [this](std::unique_ptr<SessionBase> s, const boost::system::error_code &ec){
                if (ec) {
                    std::cerr << "An error occurred when sending error response: " << ec.message() << std::endl;
                } else
                    this->handle_error(std::move(s), "TraditionalSmtpsFsm::handle_greeting_ehlo: 501 Syntax error in parameters or arguments");
            });
        return;
    }

    // 发送支持的SMTP扩展
    std::string response = "250-" + args + " Hello\r\n"
                          "250-SIZE 10240000\r\n"  // 10MB 最大消息大小
                          "250-8BITMIME\r\n"
                          "250 SMTPUTF8\r\n";
    SessionBase::async_write(std::move(session), response, 
        [this](std::unique_ptr<SessionBase> s, const boost::system::error_code &ec){
            if (!ec && s && !s->is_closed()) {
                s->set_current_state(int(SmtpsState::WAIT_AUTH));
                std::cout << "EHLO response sent, waiting for AUTH" << std::endl;
                
                // 启动异步读取等待下一条命令
                SessionBase::async_read(std::move(s), [this](std::unique_ptr<SessionBase> sss, const boost::system::error_code& read_ec, std::size_t bytes_transferred){
                    if (read_ec || !sss || sss->is_closed()) {
                        std::cerr << "Error reading after EHLO: " << (read_ec ? read_ec.message() : "Session null/closed") << std::endl;
                        return;
                    }
                    
                    template_func(std::move(sss));
                });
            } else {
                std::cerr << "Error sending EHLO response: " << (ec ? ec.message() : "Session null/closed") << std::endl;
            }
        });
}

void TraditionalSmtpsFsm::handle_wait_auth_auth(std::unique_ptr<SessionBase> session, const std::string& args) {
    // 处理AUTH命令
    if (args.empty()) {
        SessionBase::async_write(std::move(session), "501 Syntax error in parameters or arguments\r\n", 
            [this](std::unique_ptr<SessionBase> s, const boost::system::error_code &ec){
                if (ec) {
                    std::cerr << "An error occurred when sending auth error response: " << ec.message() << std::endl;
                }
                this->handle_error(std::move(s), "TraditionalSmtpsFsm::handle_wait_auth_auth: 501 Syntax error in parameters or arguments");
            });
        return;
    }

    // 发送认证请求
    SessionBase::async_write(std::move(session), "334 VXNlcm5hbWU6\r\n", 
        [](std::unique_ptr<SessionBase> s, const boost::system::error_code &ec){
            if (!ec) {
                s->set_current_state(int(SmtpsState::WAIT_AUTH_USERNAME));
                SessionBase::async_read(std::move(s), [](std::unique_ptr<SessionBase> sss, const boost::system::error_code& read_ec, std::size_t bytes_transferred){
                    if (read_ec || !sss || sss->is_closed()) {
                        std::cerr << "Error reading after AUTH: " << (read_ec ? read_ec.message() : "Session null/closed") << std::endl;
                        return;
                    }
                    template_func(std::move(sss));
                });
            } else {
                std::cerr << "An error occurred when sending username request: " << ec.message() << std::endl;
            }
        }); // "Username:" in base64
}

void TraditionalSmtpsFsm::handle_wait_auth_username(std::unique_ptr<SessionBase> session, const std::string& args) {
    // 保存用户名
    static_cast<SmtpsContext*>(session->get_context())->client_username = args;
    SessionBase::async_write(std::move(session), "334 UGFzc3dvcmQ6\r\n", 
        [this](std::unique_ptr<SessionBase> s, const boost::system::error_code &ec){
            if (!ec) {
                s->set_current_state(int(SmtpsState::WAIT_AUTH_PASSWORD));
            } else {
                this->handle_error(std::move(s), "TraditionalSmtpsFsm::handle_wait_auth_username: 501 Syntax error in parameters or arguments");
            }
        }); // "Password:" in base64
}

void TraditionalSmtpsFsm::handle_wait_auth_password(std::unique_ptr<SessionBase> session, const std::string& args) {
    // 验证用户名和密码
    // 这里需要重新获取会话上下文，因为session已经被移动
    std::string username = static_cast<SmtpsContext*>(session->get_context())->client_username;
    if (auth_user(session.get(), username, args)) {
        static_cast<SmtpsContext*>(session->get_context())->is_authenticated = true;
        SessionBase::async_write(std::move(session), "235 Authentication successful\r\n", 
            [this](std::unique_ptr<SessionBase> s, const boost::system::error_code &ec){
                if (!ec) {
                    s->set_current_state(int(SmtpsState::WAIT_MAIL_FROM));
                } else {
                    this->handle_error(std::move(s), "TraditionalSmtpsFsm::handle_wait_auth_password: 501 Syntax error in parameters or arguments");
                }
            });
    } else {
        SessionBase::async_write(std::move(session), "535 Authentication failed\r\n", 
            [this](std::unique_ptr<SessionBase> s, const boost::system::error_code &ec){
                if (!ec) {
                    this->handle_error(std::move(s), "Authentication failed");
                }
            });
    }
}

void TraditionalSmtpsFsm::handle_wait_auth_mail_from(std::unique_ptr<SessionBase> session, const std::string& args) {
    // 检查是否需要强制认证（这里可以根据配置或其他条件来决定）
    bool require_auth = false; // 默认不强制认证
    std::cout << "DEBUG: handle_wait_auth_mail_from called, require_auth=" << require_auth << std::endl;

    // 如果需要强制认证但客户端未认证
    if (require_auth && !static_cast<SmtpsContext*>(session->get_context())->is_authenticated) {
        // 发送认证要求
        SessionBase::async_write(std::move(session), "530 Authentication required\r\n", 
            [](std::unique_ptr<SessionBase> s, const boost::system::error_code &ec){
                if (ec) {
                    std::cerr << "An error occurred when sending auth required: " << ec.message() << std::endl;
                }
            });
        return;
    }
    
    // 处理MAIL FROM命令，与handle_wait_mail_from_mail_from相同
    std::regex mail_from_regex(R"(FROM:\s*<([^>]*)>)", std::regex_constants::icase);
    std::smatch matches;
    if (std::regex_search(args, matches, mail_from_regex) && matches.size() > 1) {
        // 保存发件人地址
        static_cast<SmtpsContext*>(session->get_context())->sender_address = matches[1];
        SessionBase::async_write(std::move(session), "250 Ok\r\n", 
            [this](std::unique_ptr<SessionBase> s, const boost::system::error_code &ec){
                if (!ec) {
                    s->set_current_state(int(SmtpsState::WAIT_RCPT_TO));
                } else {
                    std::cerr << "An error occurred when sending mail from ok: " << ec.message() << std::endl;
                }
                SessionBase::async_read(std::move(s), [this](std::unique_ptr<SessionBase> sss, const boost::system::error_code& read_ec, std::size_t bytes_transferred){
                    if (read_ec || !sss || sss->is_closed()) {
                        std::cerr << "Error reading after MAIL FROM: " << (read_ec ? read_ec.message() : "Session null/closed") << std::endl;
                        return;
                    }
                    template_func(std::move(sss));
                });
            });
    } else {
        SessionBase::async_write(std::move(session), "501 Syntax error in parameters or arguments\r\n", 
            [](std::unique_ptr<SessionBase> s, const boost::system::error_code &ec){
                if (ec) {
                    std::cerr << "An error occurred when sending mail from syntax error: " << ec.message() << std::endl;
                }
            });
    }
}

void TraditionalSmtpsFsm::handle_wait_mail_from_mail_from(std::unique_ptr<SessionBase> session, const std::string& args) {
    // 解析MAIL FROM命令
    std::regex mail_from_regex(R"(FROM:\s*<([^>]*)>)", std::regex_constants::icase);
    std::smatch matches;
    if (std::regex_search(args, matches, mail_from_regex) && matches.size() > 1) {
        // 保存发件人地址
        static_cast<SmtpsContext*>(session->get_context())->sender_address = matches[1];
        SessionBase::async_write(std::move(session), "250 Ok\r\n", 
            [](std::unique_ptr<SessionBase> s, const boost::system::error_code &ec){
                if (!ec) {
                    s->set_current_state(int(SmtpsState::WAIT_RCPT_TO));
                    SessionBase::async_read(std::move(s), [](std::unique_ptr<SessionBase> sss, const boost::system::error_code& read_ec, std::size_t bytes_transferred){
                        if (read_ec || !sss || sss->is_closed()) {
                            std::cerr << "Error reading after MAIL FROM: " << (read_ec ? read_ec.message() : "Session null/closed") << std::endl;
                            return;
                        }
                        template_func(std::move(sss));
                    });
                } else {
                    std::cerr << "An error occurred when sending mail from ok: " << ec.message() << std::endl;
                }
            });
    } else {
        SessionBase::async_write(std::move(session), "501 Syntax error in parameters or arguments\r\n", 
            [this](std::unique_ptr<SessionBase> s, const boost::system::error_code &ec){
                if (ec) {
                    std::cerr << "An error occurred when sending mail from syntax error: " << ec.message() << std::endl;
                } else
                    this->handle_error(std::move(s), "TraditionalSmtpsFsm::handle_wait_mail_from_mail_from: 501 Syntax error in parameters or arguments");
            });
    }
}

void TraditionalSmtpsFsm::handle_wait_rcpt_to_rcpt_to(std::unique_ptr<SessionBase> session, const std::string& args) {
    // 解析RCPT TO命令
    std::regex rcpt_to_regex(R"(TO:\s*<([^>]*)>)", std::regex_constants::icase);
    std::smatch matches;
    if (std::regex_search(args, matches, rcpt_to_regex) && matches.size() > 1) {
        for(auto& match : matches)
            static_cast<SmtpsContext*>(session->get_context())->recipient_addresses.push_back(match);
        SessionBase::async_write(std::move(session), "250 Ok\r\n", 
            [this](std::unique_ptr<SessionBase> s, const boost::system::error_code &ec){
                if (!ec) {
                    s->set_current_state(int(SmtpsState::WAIT_DATA));
                    SessionBase::async_read(std::move(s), [this](std::unique_ptr<SessionBase> sss, const boost::system::error_code& read_ec, std::size_t bytes_transferred){
                        if (read_ec || !sss || sss->is_closed()) {
                            std::cerr << "Error reading after RCPT TO: " << (read_ec ? read_ec.message() : "Session null/closed") << std::endl;
                            return;
                        }
                        template_func(std::move(sss));
                    });
                } else {
                    std::cerr << "An error occurred when sending rcpt to ok: " << ec.message() << std::endl;
                }
            });
    } else {
        SessionBase::async_write(std::move(session), "501 Syntax error in parameters or arguments\r\n", 
            [this](std::unique_ptr<SessionBase> s, const boost::system::error_code &ec){
                if (ec) {
                    std::cerr << "An error occurred when sending rcpt to syntax error: " << ec.message() << std::endl;
                } else
                    this->handle_error(std::move(s), "TraditionalSmtpsFsm::handle_wait_rcpt_to_rcpt_to: 501 Syntax error in parameters or arguments");
            });
    }
}

void TraditionalSmtpsFsm::handle_wait_data_data(std::unique_ptr<SessionBase> session, const std::string& args) {
    SessionBase::async_write(std::move(session), "354 Start mail input; end with <CRLF>.<CRLF>\r\n", 
        [this](std::unique_ptr<SessionBase> s, const boost::system::error_code& ec){
            if (!ec) {
                s->set_current_state(int(SmtpsState::IN_MESSAGE));
                SessionBase::async_read(std::move(s), [this](std::unique_ptr<SessionBase> sss, const boost::system::error_code& read_ec, std::size_t bytes_transferred){
                    if (read_ec || !sss || sss->is_closed()) {
                        std::cerr << "Error reading after DATA: " << (read_ec ? read_ec.message() : "Session null/closed") << std::endl;
                        return;
                    }
                    template_func(std::move(sss));
                });
            } else {
                std::cerr << "An error occurred when sending data ready response: " << ec.message() << std::endl;
            }
        });
}

void TraditionalSmtpsFsm::handle_in_message_data(std::unique_ptr<SessionBase> session, const std::string& args) {
    std::cout << "keep receiving data" << std::endl;
    SessionBase::async_read(std::move(session), [this](std::unique_ptr<SessionBase> sss, const boost::system::error_code& read_ec, std::size_t bytes_transferred){
        if (read_ec || !sss || sss->is_closed()) {
            std::cerr << "Error reading in IN_MESSAGE state: " << (read_ec ? read_ec.message() : "Session null/closed") << std::endl;
            return;
        }
        template_func(std::move(sss));
    });
}

void TraditionalSmtpsFsm::handle_in_message_data_end(std::unique_ptr<SessionBase> session, const std::string& args) {
    SessionBase::async_write(std::move(session), "250 Message accepted for delivery\r\n", 
        [this](std::unique_ptr<SessionBase> s, const boost::system::error_code &ec){
            if (!ec) {
                s->set_current_state(int(SmtpsState::WAIT_QUIT));
                SessionBase::async_read(std::move(s), [this](std::unique_ptr<SessionBase> sss, const boost::system::error_code& read_ec, std::size_t bytes_transferred){
                    if (read_ec || !sss || sss->is_closed()) {
                        std::cerr << "Error reading after DATA: " << (read_ec ? read_ec.message() : "Session null/closed") << std::endl;
                        return;
                    }
                    template_func(std::move(sss));
                });
            } else {
                std::cerr << "An error occurred when sending message accepted: " << ec.message() << std::endl;
            }
        });
}

void TraditionalSmtpsFsm::handle_wait_quit_quit(std::unique_ptr<SessionBase> session, const std::string& args) {
    // 先保存邮件数据
    
    SessionBase::async_write(std::move(session), "221 Bye\r\n", 
        [this](std::unique_ptr<SessionBase> s, const boost::system::error_code& ec){
            if (!ec) {
                // 保存和转发邮件
                // if (mail_ptr) {
                //     server->start_forward_email(mail_ptr);
                //     m_workerThreadPool->post([this, mail_ptr](){save_mail_data(mail_ptr);});
                // }
                
                // 清理上下文数据并关闭连接
                static_cast<SmtpsContext*>(s->get_context())->clear();
                s->close();
            } else {
                std::cerr << "An error occurred when sending quit response: " << ec.message() << std::endl;
                s->close();
            }
        });
}

void TraditionalSmtpsFsm::handle_error(std::unique_ptr<SessionBase> session, const std::string& args) {
    session->stay_times++;
    if(session->stay_times > 3) {
        std::cout << "Too many errors, closing session." << std::endl;
        session->close();
    } else {
        std::string client_ip = session->get_client_ip();
        SmtpsState current_state = static_cast<SmtpsState>(session->get_current_state());
        SmtpsEvent last_event = static_cast<SmtpsEvent>(session->get_next_event());
        SessionBase::async_write(std::move(session), "500 Error: " + args + "\r\n", 
            [this](std::unique_ptr<SessionBase> s, const boost::system::error_code &ec){
                if (!ec) {
                    // 继续等待下一条命令
                    SessionBase::async_read(std::move(s), [this](std::unique_ptr<SessionBase> sss, const boost::system::error_code& read_ec, std::size_t bytes_transferred){
                        if (read_ec || !sss || sss->is_closed()) {
                            std::cerr << "Error reading after error handling: " << (read_ec ? read_ec.message() : "Session null/closed") << std::endl;
                            return;
                        }
                        template_func(std::move(sss));
                    });
                } else {
                    std::cerr << "An error occurred when sending error response: " << ec.message() << std::endl;
                }
            });
            std::cout << "Error occurred in session from " << client_ip 
                      << " at state " << get_state_name(current_state) 
                      << " on event " << get_event_name(last_event) 
                      << ": " << args << std::endl;
    }
}

} // namespace mail_system