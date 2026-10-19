#ifndef PR_FRAMEWORK_SSL_CONNECTION_H
#define PR_FRAMEWORK_SSL_CONNECTION_H
#include "framework/connection/connection_handle.h"
namespace pr {
// Protocol-facing TLS connection; implementation supplied by the network backend.
class SslConnection final : public ConnectionHandle {
public:
    using ConnectionHandle::ConnectionHandle;
};
} // namespace pr
#endif
