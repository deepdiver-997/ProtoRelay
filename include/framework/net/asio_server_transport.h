#ifndef PR_FRAMEWORK_NET_ASIO_SERVER_TRANSPORT_H
#define PR_FRAMEWORK_NET_ASIO_SERVER_TRANSPORT_H
#include "framework/net/server_transport.h"
#include <boost/asio/any_io_executor.hpp>
namespace pr::net {
std::unique_ptr<IServerTransport> make_asio_server_transport(const ServerTransportConfig& config,
    std::function<boost::asio::any_io_executor()> next_executor);
}
#endif
