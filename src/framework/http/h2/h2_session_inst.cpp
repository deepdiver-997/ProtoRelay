// 显式模板实例化 — H2Session（TCP + SSL）
#include "framework/http/h2/h2_session.h"
#include "framework/http/h2/h2_session.tpp"
#include "framework/connection/tcp_connection.h"
#include "framework/connection/ssl_connection.h"

namespace pr::http {
namespace h2 {

template class H2Session<pr::TcpConnection>;
template class H2Session<pr::SslConnection>;

} // namespace h2
} // namespace pr::http