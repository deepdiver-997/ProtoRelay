#ifndef I_CONNECTION_H
#define I_CONNECTION_H

#include <boost/asio.hpp>
#include <functional>
#include <memory>
#include <string>

namespace mail_system {

template <typename ConnectionType>
class SessionBase;

// 连接抽象接口，隐藏底层 socket 细节
class IConnection {
public:
    using ReadHandler = std::function<void(const boost::system::error_code&, std::size_t)>;
    using WriteHandler = std::function<void(const boost::system::error_code&, std::size_t)>;
    using HandshakeHandler = std::function<void(const boost::system::error_code&)>;

    virtual ~IConnection() = default;

    // 异步读取
    virtual void async_read(
        boost::asio::mutable_buffer buffer,
        ReadHandler handler
    ) = 0;

    // 异步写入
    virtual void async_write(
        boost::asio::const_buffer buffer,
        WriteHandler handler
    ) = 0;

    // SSL 握手（TCP 连接直接调用成功回调）
    virtual void async_handshake(
        boost::asio::ssl::stream_base::handshake_type type,
        HandshakeHandler handler
    ) = 0;

    // 关闭连接
    virtual void close() = 0;

    // 检查连接是否打开
    virtual bool is_open() const = 0;

    // 获取本地端口（隐藏底层细节）
    virtual uint16_t get_local_port() const = 0;

    // 获取远程 IP 地址
    virtual std::string get_remote_ip() const = 0;

    // 获取原始 socket 指针（谨慎使用，仅用于类型转换）
    virtual std::unique_ptr<boost::asio::ip::tcp::socket> release_socket() = 0;
};

} // namespace mail_system

#endif // I_CONNECTION_H
