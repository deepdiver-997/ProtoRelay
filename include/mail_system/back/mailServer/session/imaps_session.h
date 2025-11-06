#ifndef IMAPS_SESSION_H
#define IMAPS_SESSION_H

#include "session_base.h"
#include <string>
#include <memory>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

namespace mail_system {

// IMAP状态枚举
enum class ImapsState {
    INIT,               // 初始状态
    GREETING,          // 发送问候语
    WAIT_LOGIN,        // 等待LOGIN命令
    WAIT_AUTHENTICATE, // 等待AUTHENTICATE命令
    WAIT_SELECT,       // 等待SELECT命令
    WAIT_FETCH,        // 等待FETCH命令
    WAIT_STORE,        // 等待STORE命令
    WAIT_EXPUNGE,      // 等待EXPUNGE命令
    WAIT_CLOSE,        // 等待CLOSE命令
    WAIT_LOGOUT,       // 等待LOGOUT命令
    CLOSED
};

// IMAP事件枚举
enum class ImapsEvent {
    CONNECT,          // 连接建立
    LOGIN,            // 收到LOGIN命令
    AUTHENTICATE,     // 收到AUTHENTICATE命令
    SELECT,           // 收到SELECT命令
    READ_WRITE,       // 读写模式
    FETCH,            // 收到FETCH命令
    STORE,            // 收到STORE命令
    EXPUNGE,          // 收到EXPUNGE命令
    CLOSE,            // 收到CLOSE命令
    LOGOUT,           // 收到LOGOUT命令
    ERROR,            // 发生错误
    TIMEOUT           // 超时
};

// IMAP会话上下文
struct ImapsContext {
    std::string client_hostname;     // 客户端主机名
    std::string client_username;     // 客户端用户名
    std::string client_password;     // 客户端密码
    std::string selected_mailbox;    // 当前选中的邮箱
    std::string cmd_tag;             // 当前命令标签
    bool is_authenticated;           // 是否已认证
    
    // 清理上下文数据
    void clear() {
        client_hostname.clear();
        client_username.clear();
        selected_mailbox.clear();
        is_authenticated = false;
    }
};

// 前向声明
class ImapsFsm;

class ImapsSession : public SessionBase {
public:
    ImapsSession(ServerBase* server, std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> &&socket, std::shared_ptr<ImapsFsm> fsm);
    ~ImapsSession() override;

    // 启动会话
    void start() override;

    ImapsState get_current_state() const {
        return current_state_;
    }

    void set_current_state(ImapsState state) {
        current_state_ = state;
        stay_times = 0;
    }

protected:
    // 处理接收到的数据
    void handle_read(const std::string& data) override;
    
    void process_command(const std::string& command);

public:
    ImapsContext context_;           // 会话上下文

    int stay_times;
    int timeout_times;
private:
    std::shared_ptr<ImapsFsm> m_fsm;  // 状态机
    ImapsState current_state_;      // 当前状态
    bool m_receivingData;            // 是否在接收数据模式
};

} // namespace mail_system

#endif // IMAPS_SESSION_H