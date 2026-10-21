#pragma once
#include "framework/net/readiness_waiter.h"
#include "framework/net/executor.h"

namespace pr::net {
// Adapter factory: rejects a non-Asio executor without exposing Boost in the driver API.
std::shared_ptr<IReadinessWaiter> make_asio_readiness_waiter(std::shared_ptr<IExecutor> executor);
}
