// 显式模板实例化 — pr::SessionBase
// 避免每个翻译单元重复编译 session_base.tpp（278 行）
#include "framework/session_base.h"
#include "framework/session_base.tpp"
#include "framework/connection/tcp_connection.h"
#include "framework/connection/ssl_connection.h"

namespace pr {

template class SessionBase<TcpConnection>;
template class SessionBase<SslConnection>;

} // namespace pr
