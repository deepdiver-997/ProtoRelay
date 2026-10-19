#ifndef PR_FRAMEWORK_NET_ASIO_CONNECTION_H
#define PR_FRAMEWORK_NET_ASIO_CONNECTION_H
// Driver-specific construction helpers; never included by protocol interfaces.
#include "framework/connection/i_connection.h"
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
namespace pr::net {
std::unique_ptr<IConnection> make_asio_tcp_connection(std::unique_ptr<boost::asio::ip::tcp::socket> socket);
std::unique_ptr<IConnection> make_asio_ssl_connection(std::unique_ptr<boost::asio::ip::tcp::socket> socket,
    std::shared_ptr<boost::asio::ssl::context> context);
std::unique_ptr<IConnection> upgrade_asio_tls(std::unique_ptr<IConnection> connection,
    std::shared_ptr<boost::asio::ssl::context> context);
}
#endif
