// 显式模板实例化 — TraditionalImapsFsm
#include "mail_system/back/mailServer/fsm/imaps/traditional_imaps_fsm.h"
#include "mail_system/back/mailServer/fsm/imaps/traditional_imaps_fsm.tpp"
#include "framework/connection/tcp_connection.h"
#include "framework/connection/ssl_connection.h"

namespace mail_system {

template class TraditionalImapsFsm<pr::TcpConnection>;
template class TraditionalImapsFsm<pr::SslConnection>;

} // namespace mail_system
