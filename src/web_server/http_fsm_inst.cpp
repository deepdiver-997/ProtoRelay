// 显式模板实例化 — HttpFsm（TCP + SSL）
#include "web_server/http_fsm.h"
#include "web_server/http_fsm.tpp"
#include "framework/connection/tcp_connection.h"
#include "framework/connection/ssl_connection.h"

namespace web_server {

template class HttpFsm<pr::TcpConnection>;
template class HttpFsm<pr::SslConnection>;

} // namespace web_server