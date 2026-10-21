#ifndef PR_FRAMEWORK_HTTP_H2_SERVER_H
#define PR_FRAMEWORK_HTTP_H2_SERVER_H
// ──────────────────────────────────────────────────────────────────
// H2Server — HTTP/2 服务器装配（复用 pr::TcpServerBase 工厂）
// 与 HttpServer 同构，只是 session 换成 H2Session（多流帧式）.
// ──────────────────────────────────────────────────────────────────
#include "framework/tcp_server_base.h"
#include "framework/http/h2/h2_session.h"
#include <memory>
#include <string>

namespace pr::http {
namespace h2 {

class H2Server : public pr::TcpServerBase<TcpH2Session, SslH2Session> {
public:
    H2Server(const pr::ServerConfig& config,
             std::string doc_root,
             std::shared_ptr<pr::ThreadPoolBase> ioThreadPool,
             std::shared_ptr<pr::ThreadPoolBase> workerThreadPool);
    ~H2Server() override { stop(); }

    const std::string& doc_root() const { return doc_root_; }

protected:
    std::shared_ptr<TcpH2Session> make_tcp_session(
        std::unique_ptr<pr::TcpConnection> conn,
        const pr::ListenerConfig& lc) override;
    std::shared_ptr<SslH2Session> make_ssl_session(
        std::unique_ptr<pr::SslConnection> conn,
        const pr::ListenerConfig& lc) override;
    bool should_reject_connection(std::string& reason,
        const std::string& client_ip = "") const override;

private:
    std::string doc_root_;
};

} // namespace h2
} // namespace pr::http

#endif // PR_FRAMEWORK_HTTP_H2_SERVER_H