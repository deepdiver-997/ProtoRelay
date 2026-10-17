// 显式模板实例化 — HttpFsm（TCP + SSL）
#include "framework/http/http_fsm.h"
#include "framework/http/http_fsm.tpp"
#include "framework/connection/tcp_connection.h"
#include "framework/connection/ssl_connection.h"

namespace pr::http {

template class HttpFsm<pr::TcpConnection>;
template class HttpFsm<pr::SslConnection>;

} // namespace pr::http