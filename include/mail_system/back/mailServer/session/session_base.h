#ifndef SESSION_BASE_H
#define SESSION_BASE_H

#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <mail_system/back/entities/mail.h>
#include <mail_system/back/entities/usr.h>
#include <mail_system/back/mailServer/server_base.h>

// #define _LIBCPP_STD_VER 17

namespace mail_system {
class ServerBase;
class SessionBase : public std::enable_shared_from_this<SessionBase> {
public:
    // 构造函数
    SessionBase(std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> &&, ServerBase*);

    SessionBase(SessionBase&&);

    // 虚析构函数
    virtual ~SessionBase();

    // 启动会话
    // virtual void start(std::unique_ptr<SessionBase> self) = 0;

    // 关闭会话
    virtual void close();

    // 获取客户端地址
    std::string get_client_ip() const;

    boost::asio::ssl::stream<boost::asio::ip::tcp::socket>& get_ssl_socket();

    mail* get_mail();

    std::unique_ptr<mail> get_mail_ptr() {
        auto m = std::move(mail_);
        mail_ = nullptr;
        return std::move(m);
    }
    
    // 设置服务器引用
    void set_server(ServerBase* server);

    // 执行SSL握手
    static void do_handshake(std::unique_ptr<SessionBase> self, std::function<void(std::unique_ptr<SessionBase> s, const boost::system::error_code&)> callback = nullptr);


    // 异步读取数据 如果存在回调函数则把self转移交给回调函数处理，否则直接退出
    static void async_read(std::unique_ptr<SessionBase> self, std::function<void(std::unique_ptr<SessionBase> s, const boost::system::error_code&, std::size_t)> callback = nullptr);

    // 异步写入数据 如果存在回调函数则把self转移交给回调函数处理，否则继续读取
    static void async_write(std::unique_ptr<SessionBase> self, const std::string& data, std::function<void(std::unique_ptr<SessionBase> s, const boost::system::error_code&)> callback = nullptr);

    // 处理接收到的数据（由派生类实现）
    virtual void handle_read() = 0;

    virtual void set_current_state(int) = 0;

    virtual void set_next_event(int) = 0;

    virtual void* get_fsm() const = 0;

    virtual int get_next_event() const = 0;

    virtual int get_current_state() const = 0;

    virtual void* get_context() = 0;

    // 处理错误
    virtual void handle_error(const boost::system::error_code& error);

    // 会话是否已关闭
    bool is_closed() const;
    
    // 获取最后读取的数据（用于状态机回调）
    std::string get_last_read_data(size_t bytes_transferred);

    // 最近一次异步读取传输的字节数
    size_t last_bytes_transferred_ = 0;

protected:

    // // IO上下文引用
    // boost::asio::io_context& m_io_context;
    
    // // SSL上下文引用
    // boost::asio::ssl::context& m_ssl_context;
    
    // SSL流
    std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> m_socket;

    // 读取缓冲区
    std::vector<char> read_buffer_, use_buffer_;

    // 客户端地址
    mutable std::string client_address_;

    std::unique_ptr<mail> mail_;

    std::unique_ptr<usr> usr_;

    // 会话是否已关闭
    bool closed_;

    public:
    int stay_times;
    int timeout_times;
    // 指向服务器的指针，用于访问IO线程池
    ServerBase* m_server;
};

} // namespace mail_system

#endif // SESSION_BASE_H