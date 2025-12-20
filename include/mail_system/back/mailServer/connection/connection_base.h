#ifndef CONNECTION_BASE_H
#define CONNECTION_BASE_H

#include <memory>
#include <string>
#include <boost/asio.hpp>
#ifdef USE_SSL
#include <boost/asio/ssl.hpp>
#endif

namespace mail_system {

// 前向声明，用于特化
template<typename SocketType>
class ConnectionBase;

// SSL连接类型别名
#ifdef USE_SSL
using SSLConnection = ConnectionBase<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>;
#endif

// TCP连接类型别名
using TCPConnection = ConnectionBase<boost::asio::ip::tcp::socket>;

// 模板特化实现
template<typename SocketType>
class ConnectionBase {
public:
    explicit ConnectionBase(std::unique_ptr<SocketType>&& socket)
        : socket_(std::move(socket)) {}

    virtual ~ConnectionBase() = default;

    // 获取底层socket引用
    SocketType& get_socket() {
        return *socket_;
    }

    // 获取底层socket常量引用
    const SocketType& get_socket() const {
        return *socket_;
    }

    // 异步读取数据
    template<typename ReadHandler>
    void async_read(boost::asio::mutable_buffer buffer, ReadHandler&& handler) {
        get_socket().async_read_some(buffer, std::forward<ReadHandler>(handler));
    }

    // 异步写入数据
    template<typename WriteHandler>
    void async_write(const boost::asio::const_buffer& buffer, WriteHandler&& handler) {
        boost::asio::async_write(get_socket(), buffer, std::forward<WriteHandler>(handler));
    }

    // 获取远程端点
    std::string get_remote_endpoint() const {
        boost::system::error_code ec;
        auto endpoint = get_socket().lowest_layer().remote_endpoint(ec);
        if (ec) {
            return "unknown";
        }
        return endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
    }

    // 关闭连接
    void close() {
        boost::system::error_code ec;
        get_socket().lowest_layer().close(ec);
    }

    // 检查连接是否打开
    bool is_open() const {
        return get_socket().lowest_layer().is_open();
    }

protected:
    std::unique_ptr<SocketType> socket_;
};

// SSL特化版本，添加握手功能
#ifdef USE_SSL
template<>
class ConnectionBase<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> {
public:
    explicit ConnectionBase(std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>&& socket)
        : socket_(std::move(socket)) {}

    virtual ~ConnectionBase() = default;

    // 获取底层socket引用
    boost::asio::ssl::stream<boost::asio::ip::tcp::socket>& get_socket() {
        return *socket_;
    }

    // 获取底层socket常量引用
    const boost::asio::ssl::stream<boost::asio::ip::tcp::socket>& get_socket() const {
        return *socket_;
    }

    // 异步读取数据
    template<typename ReadHandler>
    void async_read(boost::asio::mutable_buffer buffer, ReadHandler&& handler) {
        get_socket().async_read_some(buffer, std::forward<ReadHandler>(handler));
    }

    // 异步写入数据
    template<typename WriteHandler>
    void async_write(const boost::asio::const_buffer& buffer, WriteHandler&& handler) {
        boost::asio::async_write(get_socket(), buffer, std::forward<WriteHandler>(handler));
    }

    // 获取远程端点
    std::string get_remote_endpoint() const {
        boost::system::error_code ec;
        auto endpoint = get_socket().lowest_layer().remote_endpoint(ec);
        if (ec) {
            return "unknown";
        }
        return endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
    }

    // 关闭连接
    void close() {
        boost::system::error_code ec;
        get_socket().lowest_layer().close(ec);
    }

    // 检查连接是否打开
    bool is_open() const {
        return get_socket().lowest_layer().is_open();
    }

    // SSL握手
    template<typename HandshakeHandler>
    void async_handshake(boost::asio::ssl::stream_base::handshake_type type, HandshakeHandler&& handler) {
        get_socket().async_handshake(type, std::forward<HandshakeHandler>(handler));
    }

protected:
    std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> socket_;
};
#endif

} // namespace mail_system

#endif // CONNECTION_BASE_H