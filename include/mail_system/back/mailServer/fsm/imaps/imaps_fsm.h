#ifndef IMAPS_FSM_H
#define IMAPS_FSM_H

#include "mail_system/back/mailServer/session/imaps_session.h"
#include "mail_system/back/db/db_pool.h"
#include "mail_system/back/db/db_service.h"
#include "mail_system/back/thread_pool/thread_pool_base.h"
#include <functional>
#include <map>
#include <string>
#include <regex>

namespace mail_system {

// 状态处理函数类型定义
using StateHandler = std::function<void(std::weak_ptr<ImapsSession>, const std::vector<std::string>&)>;

// IMAPS状态机接口
class ImapsFsm {
protected:
    std::shared_ptr<ThreadPoolBase> m_ioThreadPool;
    std::shared_ptr<ThreadPoolBase> m_workerThreadPool;
    std::shared_ptr<DBPool> m_dbPool;
public:
    ImapsFsm(std::shared_ptr<ThreadPoolBase> io_thread_pool,
             std::shared_ptr<ThreadPoolBase> worker_thread_pool,
             std::shared_ptr<DBPool> db_pool)
        : m_ioThreadPool(io_thread_pool),
          m_workerThreadPool(worker_thread_pool),
          m_dbPool(db_pool) {}
    virtual ~ImapsFsm() = default;

    // 处理事件
    virtual void process_event(std::weak_ptr<ImapsSession> session, ImapsEvent event, const std::vector<std::string>& args) = 0;

    // 获取状态名称
    static std::string get_state_name(ImapsState state);

    // 获取事件名称
    static std::string get_event_name(ImapsEvent event);

    // 数据库操作
    bool auth_user(std::weak_ptr<ImapsSession> session, const std::string& username, const std::string& password) {
        auto s = session.lock();
        if (!s) {
            std::cerr << "Session is expired in auth_user" << std::endl;
            return false;
        }
        auto connection = m_dbPool->get_connection();
        if (connection && connection->is_connected()) {
            std::string sql = "SELECT * FROM users WHERE username = '" +
                              connection->escape_string(username) + "' AND password = '" +
                              connection->escape_string(password) + "'";
            auto result = connection->query(sql);
            return result->get_row_count();
        }
        return false;
    }

    void get_mailbox_data(std::weak_ptr<ImapsSession> session, std::string& mailbox_data) {
        auto s = session.lock();
        if (!s) {
            std::cerr << "Session is expired in get_mailbox_data" << std::endl;
            return;
        }
        auto connection = m_dbPool->get_connection();
        if (connection && connection->is_connected()) {
            std::string sql = "SELECT mail_data FROM mails WHERE mailbox = '" +
                              connection->escape_string(s->context_.selected_mailbox) + "'";
            auto result = connection->query(sql);
            if (!result->get_row_count()) {
                mailbox_data = result->get_value(0, "mail_data");
            }
        }
    }

    void save_mail_data(mail* d, const std::string& box_name) {
        std::unique_ptr<mail> data;
        data.reset(d);
        auto connection = m_dbPool->get_connection();
        if (connection && connection->is_connected()) {
            std::string sql = "INSERT INTO mails (sender, recipient, subject, body, mailbox) VALUES ('" +
                              connection->escape_string(data->from) + "', '" +
                              connection->escape_string(data->to) + "', '" +
                              connection->escape_string(data->header) + "', '" +
                              connection->escape_string(data->body) + "', '" +
                              connection->escape_string(box_name) + "')";
            connection->execute(sql);
        }
    }
};

} // namespace mail_system

#endif // IMAPS_FSM_H