#ifndef TRADITIONAL_SMTPS_FSM_H
#define TRADITIONAL_SMTPS_FSM_H

#include "mail_system/back/mailServer/session/session_base.h"
#include "mail_system/back/db/db_pool.h"
#include "mail_system/back/thread_pool/thread_pool_base.h"
#include "mail_system/back/mailServer/fsm/smtps/smtps_fsm.hpp"
#include "mail_system/back/persist_storage/persistent_queue.h"
#include "mail_system/back/common/logger.h"
#include "mail_system/back/algorithm/snow.h"
#include "mail_system/back/algorithm/smtp_utils.h"
#include <boost/asio/ssl.hpp>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <regex>
#include <string>
#include <unordered_map>

namespace mail_system {

template <typename ConnectionType>
class TraditionalSmtpsFsm : public SmtpsFsm<ConnectionType> {
private:
    using StateTransitionTable = std::map<std::pair<SmtpsState, SmtpsEvent>, SmtpsState>;

    StateTransitionTable transition_table_;
    std::map<SmtpsState, std::map<SmtpsEvent, StateHandler<ConnectionType>>> state_handlers_;

public:
    TraditionalSmtpsFsm(
        std::shared_ptr<ThreadPoolBase> io_thread_pool,
        std::shared_ptr<ThreadPoolBase> worker_thread_pool,
        std::shared_ptr<persist_storage::PersistentQueue> persistent_queue,
        std::shared_ptr<DBPool> db_pool
    ) : SmtpsFsm<ConnectionType>(io_thread_pool, worker_thread_pool, persistent_queue, db_pool) {
        init_transition_table();
        init_state_handlers();
    }

    ~TraditionalSmtpsFsm() override = default;

private:
    // 清理函数声明
    static void cleanup_streamed_attachments(SmtpsContext* ctx);
    static void cleanup_mail_files(mail* mail);

    // 持久化函数声明
    bool persist_mails_sync(SessionBase<ConnectionType>* session, std::string& error);
    bool persist_and_reply(std::unique_ptr<SessionBase<ConnectionType>> session);

    // 初始化函数声明
    void init_transition_table();
    void init_state_handlers();

public:
    // 事件处理函数声明
    void process_event(
        std::unique_ptr<SessionBase<ConnectionType>> session,
        SmtpsEvent event,
        const std::string& args
    ) override;
    void auto_process_event(std::unique_ptr<SessionBase<ConnectionType>> session);

private:
    // 各状态事件处理函数声明
    void handle_init_connect(std::unique_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_greeting_ehlo(std::unique_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_wait_auth_starttls(std::unique_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_wait_auth_auth(std::unique_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_wait_auth_username(std::unique_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_wait_auth_password(std::unique_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_wait_auth_mail_from(std::unique_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_wait_mail_from_mail_from(std::unique_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_wait_rcpt_to_rcpt_to(std::unique_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_wait_data_data(std::unique_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_in_message_data(std::unique_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_in_message_data_end(std::unique_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_wait_quit_quit(std::unique_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_error(std::unique_ptr<SessionBase<ConnectionType>> session, const std::string& args);
};

} // namespace mail_system

#include "mail_system/back/mailServer/fsm/smtps/traditional_smtps_fsm.tpp"

#endif // TRADITIONAL_SMTPS_FSM_H
