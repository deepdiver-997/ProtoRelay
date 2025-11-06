#ifndef TRADITIONAL_IMAPS_FSM_H
#define TRADITIONAL_IMAPS_FSM_H

#include "imaps_fsm.h"
#include <map>
#include <functional>

namespace mail_system {

// 传统的IMAPS状态机实现
class TraditionalImapsFsm : public ImapsFsm {
public:
    TraditionalImapsFsm(std::shared_ptr<ThreadPoolBase> io_thread_pool,
             std::shared_ptr<ThreadPoolBase> worker_thread_pool,
             std::shared_ptr<DBPool> db_pool);
    ~TraditionalImapsFsm() override = default;

    // 处理事件
    void process_event(std::weak_ptr<ImapsSession> session, ImapsEvent event, const std::vector<std::string>& args) override;


private:

    // 状态转换表类型
    using StateTransitionTable = std::map<std::pair<ImapsState, ImapsEvent>, ImapsState>;
    
    // 状态转换表
    StateTransitionTable transition_table_;

    // 状态处理函数表
    std::map<ImapsState, std::map<ImapsEvent, StateHandler>> state_handlers_;

    // 初始化状态转换表
    void init_transition_table();

    // 初始化状态处理函数
    void init_state_handlers();

    // 状态处理函数 handle_[state]_[event]
    void handle_init_connect(std::weak_ptr<ImapsSession> session, const std::vector<std::string>& args);
    void handle_greeting_login(std::weak_ptr<ImapsSession> session, const std::vector<std::string>& args);
    void handle_wait_select_select(std::weak_ptr<ImapsSession> session, const std::vector<std::string>& args);
    void handle_wait_fetch_fetch(std::weak_ptr<ImapsSession> session, const std::vector<std::string>& args);
    void handle_wait_store_store(std::weak_ptr<ImapsSession> session, const std::vector<std::string>& args);
    void handle_wait_expunge_expunge(std::weak_ptr<ImapsSession> session, const std::vector<std::string>& args);
    void handle_wait_close_close(std::weak_ptr<ImapsSession> session, const std::vector<std::string>& args);
    void handle_wait_logout_logout(std::weak_ptr<ImapsSession> session, const std::vector<std::string>& args);
    void handle_error(std::weak_ptr<ImapsSession> session, const std::vector<std::string>& args);
};

} // namespace mail_system

#endif // TRADITIONAL_IMAPS_FSM_H