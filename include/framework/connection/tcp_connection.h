#ifndef PR_FRAMEWORK_TCP_CONNECTION_H
#define PR_FRAMEWORK_TCP_CONNECTION_H
#include "framework/connection/connection_handle.h"
namespace pr {
// Protocol-facing plain TCP connection; implementation supplied by the network backend.
class TcpConnection final : public ConnectionHandle {
public:
    using ConnectionHandle::ConnectionHandle;
};
} // namespace pr
#endif
