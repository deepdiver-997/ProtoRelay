// 显式模板实例化 — HttpSession（TCP + SSL）
#include "framework/http/http_session.h"
#include "framework/http/http_session.tpp"
#include "framework/http/http_fsm.h"
#include "framework/http/http_fsm.tpp"
#include "framework/connection/tcp_connection.h"
#include "framework/connection/ssl_connection.h"

namespace pr::http {

template class HttpSession<pr::TcpConnection>;
template class HttpSession<pr::SslConnection>;

} // namespace pr::http