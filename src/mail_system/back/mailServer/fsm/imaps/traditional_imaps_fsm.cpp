#include "mail_system/back/mailServer/fsm/imaps/traditional_imaps_fsm.h"
#include <iostream>

namespace mail_system {

TraditionalImapsFsm::TraditionalImapsFsm(std::shared_ptr<ThreadPoolBase> io_thread_pool,
                                           std::shared_ptr<ThreadPoolBase> worker_thread_pool,
                                           std::shared_ptr<DBPool> db_pool)
    : ImapsFsm(io_thread_pool, worker_thread_pool, db_pool) {
    init_transition_table();
    init_state_handlers();
}

void TraditionalImapsFsm::process_event(std::weak_ptr<ImapsSession> s, ImapsEvent event, const std::vector<std::string>& args) {
    std::cout << "enter process_event\n";
    auto session = s.lock();
    if (!session) {
        std::cerr << "Session is expired in process_event" << std::endl;
        return;
    }
    if(session->get_current_state() == ImapsState::CLOSED) {
        session->close();
        return;
    }

    auto transition_key = std::make_pair(session->get_current_state(), event);
    auto transition_it = transition_table_.find(transition_key);
    
    if (transition_it != transition_table_.end()) {
        // 找到有效的状态转换
        ImapsState next_state = transition_it->second;
        
        // 查找状态处理函数
        auto state_handler_it = state_handlers_.find(session->get_current_state());
        if (state_handler_it != state_handlers_.end()) {
            auto event_handler_it = state_handler_it->second.find(event);
            if (event_handler_it != state_handler_it->second.end()) {
                // 执行状态处理函数
                m_workerThreadPool->post([session, handler = event_handler_it->second, args]() {
                    // handler(session, args);
                });
            }
        }
        else {
            std::cout << "No handler for state " << get_state_name(session->get_current_state()) << " and event " << get_event_name(event) << std::endl;
            return;
        }
        
        std::cout << "IMAPS FSM: " << get_state_name(session->get_current_state()) << " -> " 
                  << get_event_name(event) << " -> " << get_state_name(next_state) << std::endl;
    } else {
        // 无效的状态转换
        std::cerr << "IMAPS FSM: Invalid transition from " << get_state_name(session->get_current_state()) 
                  << " on event " << get_event_name(event) << std::endl;
        
        // 处理错误
        handle_error(session, std::vector<std::string>{"Invalid command sequence"});
    }
}

void TraditionalImapsFsm::init_transition_table() {
    // 初始化状态转换表
    transition_table_[std::make_pair(ImapsState::INIT, ImapsEvent::CONNECT)] = ImapsState::GREETING;
    transition_table_[std::make_pair(ImapsState::GREETING, ImapsEvent::LOGIN)] = ImapsState::WAIT_SELECT;
    transition_table_[std::make_pair(ImapsState::WAIT_SELECT, ImapsEvent::SELECT)] = ImapsState::WAIT_FETCH;
    transition_table_[std::make_pair(ImapsState::WAIT_FETCH, ImapsEvent::FETCH)] = ImapsState::WAIT_STORE;
    transition_table_[std::make_pair(ImapsState::WAIT_STORE, ImapsEvent::STORE)] = ImapsState::WAIT_EXPUNGE;
    transition_table_[std::make_pair(ImapsState::WAIT_EXPUNGE, ImapsEvent::EXPUNGE)] = ImapsState::WAIT_CLOSE;
    transition_table_[std::make_pair(ImapsState::WAIT_CLOSE, ImapsEvent::CLOSE)] = ImapsState::WAIT_LOGOUT;
    transition_table_[std::make_pair(ImapsState::WAIT_LOGOUT, ImapsEvent::LOGOUT)] = ImapsState::CLOSED;
    
    // QUIT命令可以在多个状态下接收
    for (int i = 0;i < 10; ++i)
        transition_table_[std::make_pair(static_cast<ImapsState>(i), ImapsEvent::LOGOUT)] = ImapsState::CLOSED;

    for (int i = 0;i < 10; ++i) {
        // 错误处理
        transition_table_[std::make_pair(static_cast<ImapsState>(i), ImapsEvent::ERROR)] = static_cast<ImapsState>(i);
        // 超时处理
        transition_table_[std::make_pair(static_cast<ImapsState>(i), ImapsEvent::TIMEOUT)] = static_cast<ImapsState>(i);
    }
}

void TraditionalImapsFsm::init_state_handlers() {
    // 初始化状态处理函数
    state_handlers_[ImapsState::INIT][ImapsEvent::CONNECT] = 
        std::bind(&TraditionalImapsFsm::handle_init_connect, this, std::placeholders::_1, std::placeholders::_2);

    state_handlers_[ImapsState::GREETING][ImapsEvent::LOGIN] = 
        std::bind(&TraditionalImapsFsm::handle_greeting_login, this, std::placeholders::_1, std::placeholders::_2);

    state_handlers_[ImapsState::WAIT_SELECT][ImapsEvent::SELECT] = 
        std::bind(&TraditionalImapsFsm::handle_wait_select_select, this, std::placeholders::_1, std::placeholders::_2);

    state_handlers_[ImapsState::WAIT_FETCH][ImapsEvent::FETCH] = 
        std::bind(&TraditionalImapsFsm::handle_wait_fetch_fetch, this, std::placeholders::_1, std::placeholders::_2);

    state_handlers_[ImapsState::WAIT_STORE][ImapsEvent::STORE] = 
        std::bind(&TraditionalImapsFsm::handle_wait_store_store, this, std::placeholders::_1, std::placeholders::_2);

    state_handlers_[ImapsState::WAIT_EXPUNGE][ImapsEvent::EXPUNGE] = 
        std::bind(&TraditionalImapsFsm::handle_wait_expunge_expunge, this, std::placeholders::_1, std::placeholders::_2);

    state_handlers_[ImapsState::WAIT_CLOSE][ImapsEvent::CLOSE] = 
        std::bind(&TraditionalImapsFsm::handle_wait_close_close, this, std::placeholders::_1, std::placeholders::_2);

    state_handlers_[ImapsState::WAIT_LOGOUT][ImapsEvent::LOGOUT] = 
        std::bind(&TraditionalImapsFsm::handle_wait_logout_logout, this, std::placeholders::_1, std::placeholders::_2);

    // 错误处理
    for (int i = 0;i < 11; ++i) {
        state_handlers_[static_cast<ImapsState>(i)][ImapsEvent::ERROR] = 
            std::bind(&TraditionalImapsFsm::handle_error, this, std::placeholders::_1, std::placeholders::_2);
    }
}

void TraditionalImapsFsm::handle_init_connect(std::weak_ptr<ImapsSession> session, const std::vector<std::string>& args) {
    std::cout << "handle_init_connect calling" << std::endl;
    if(auto s = session.lock())
    s->do_handshake([](std::weak_ptr<mail_system::SessionBase> session, const boost::system::error_code &ec){
        auto s = std::dynamic_pointer_cast<ImapsSession>(session.lock());
        if (!s) {
            std::cerr << "Session is expired in handle_init_connect" << std::endl;
            return;
        }
        s->set_current_state(ImapsState::GREETING);
        s->async_write("* OK IMAP4rev1 Service Ready\r\n", [s](const boost::system::error_code &e){
            if (e) {
                std::cerr << "An error occurred when sending greeting: " << e.message() << std::endl;
                return;
            }
            s->set_current_state(ImapsState::WAIT_LOGIN);
        });
    });
    else {
        std::cerr << "Session is expired in handle_init_connect" << std::endl;
        return;
    }
}

void TraditionalImapsFsm::handle_greeting_login(std::weak_ptr<ImapsSession> session, const std::vector<std::string>& args) {
    auto s = session.lock();
    if (!s) {
        std::cerr << "Session is expired in handle_greeting_login" << std::endl;
        return;
    }
    if (args.size() < 3) {
        s->async_write(s->context_.cmd_tag + " BAD Missing username or password\r\n");
        return;
    }
    s->async_write(s->context_.cmd_tag + " OK LOGIN completed\r\n");
    s->set_current_state(ImapsState::WAIT_SELECT);
}

void TraditionalImapsFsm::handle_wait_select_select(std::weak_ptr<ImapsSession> session, const std::vector<std::string>& args) {
    auto s = session.lock();
    if (!s) {
        std::cerr << "Session is expired in handle_wait_select_select" << std::endl;
        return;
    }
    s->async_write("* OK SELECT completed\r\n");
    s->set_current_state(ImapsState::WAIT_FETCH);
}

void TraditionalImapsFsm::handle_wait_fetch_fetch(std::weak_ptr<ImapsSession> session, const std::vector<std::string>& args) {
    auto s = session.lock();
    if (!s) {
        std::cerr << "Session is expired in handle_wait_fetch_fetch" << std::endl;
        return;
    }
    s->async_write("* OK FETCH completed\r\n");
    s->set_current_state(ImapsState::WAIT_STORE);
}

void TraditionalImapsFsm::handle_wait_store_store(std::weak_ptr<ImapsSession> session, const std::vector<std::string>& args) {
    auto s = session.lock();
    if (!s) {
        std::cerr << "Session is expired in handle_wait_store_store" << std::endl;
        return;
    }
    s->async_write("* OK STORE completed\r\n");
    s->set_current_state(ImapsState::WAIT_EXPUNGE);
}

void TraditionalImapsFsm::handle_wait_expunge_expunge(std::weak_ptr<ImapsSession> session, const std::vector<std::string>& args) {
    auto s = session.lock();
    if (!s) {
        std::cerr << "Session is expired in handle_wait_expunge_expunge" << std::endl;
        return;
    }
    s->async_write("* OK EXPUNGE completed\r\n");
    s->set_current_state(ImapsState::WAIT_CLOSE);
}

void TraditionalImapsFsm::handle_wait_close_close(std::weak_ptr<ImapsSession> session, const std::vector<std::string>& args) {
    auto s = session.lock();
    if (!s) {
        std::cerr << "Session is expired in handle_wait_close_close" << std::endl;
        return;
    }
    s->async_write("* OK CLOSE completed\r\n");
    s->set_current_state(ImapsState::WAIT_LOGOUT);
}

void TraditionalImapsFsm::handle_wait_logout_logout(std::weak_ptr<ImapsSession> session, const std::vector<std::string>& args) {
    auto s = session.lock();
    if (!s) {
        std::cerr << "Session is expired in handle_wait_logout_logout" << std::endl;
        return;
    }
    s->async_write("* OK LOGOUT completed\r\n");
    s->set_current_state(ImapsState::CLOSED);
}

void TraditionalImapsFsm::handle_error(std::weak_ptr<ImapsSession> session, const std::vector<std::string>& args) {
    auto s = session.lock();
    if (!s) {
        std::cerr << "Session is expired in handle_error" << std::endl;
        return;
    }
    std::string arg;
    for(const auto& a : args) {
        arg += a + " ";
    }
    s->async_write("* BAD " + arg + "\r\n");
}

} // namespace mail_system