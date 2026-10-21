#pragma once
// Driver-specific construction for composition roots and adapter tests.
#include "framework/net/client_transport.h"
#include "framework/net/asio_executor.h"
namespace pr::net {
std::shared_ptr<IClientTransport> make_asio_client_transport(std::shared_ptr<AsioExecutor> executor);
} // namespace pr::net
