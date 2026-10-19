#ifndef PR_FRAMEWORK_HTTP_HTTP_SERVER_H
#define PR_FRAMEWORK_HTTP_HTTP_SERVER_H
// ──────────────────────────────────────────────────────────────────
// HttpServer — HTTP/1.1 静态文件服务器装配
//
// 直接复用 pr::TcpServerBase（acceptor+session 工厂），对比 smtps_server 只少
// 了 DB/队列/DNS 依赖：HttpFsm 不需要线程池，纯读+静态文件发即可。
// 唯一新增：doc_root（静态根目录），在工厂里注入到每个 session。
// ──────────────────────────────────────────────────────────────────
#include "framework/tcp_server_base.h"
#include "framework/http/http_types.hpp"
#include "framework/http/http_fsm.h"
#include "framework/http/http_session.h"
#include "framework/db/db_pool.h"
#include <memory>
#include <string>

namespace pr::http {

class HttpServer : public pr::TcpServerBase<TcpHttpSession, SslHttpSession> {
public:
    HttpServer(const mail_system::ServerConfig& config,
               std::string doc_root,
               std::shared_ptr<pr::ThreadPoolBase> ioThreadPool,
               std::shared_ptr<pr::ThreadPoolBase> workerThreadPool,
               std::shared_ptr<mail_system::router::IShardRouter> shardRouter);
    ~HttpServer() override { stop(); }

    const std::string& doc_root() const { return doc_root_; }

    // 动态请求处理器（API 路由，见 ApiHandler 契约）：start() 前设置一次，
    // 工厂注入到每个新连接的 session。不设置 = 纯静态服务器（demo 默认）。
    void set_api_handler(ApiHandler h) { api_handler_ = std::move(h); }

protected:
    std::shared_ptr<TcpHttpSession> make_tcp_session(
        std::unique_ptr<pr::TcpConnection> conn,
        const mail_system::ListenerConfig& lc) override;
    std::shared_ptr<SslHttpSession> make_ssl_session(
        std::unique_ptr<pr::SslConnection> conn,
        const mail_system::ListenerConfig& lc) override;
    bool should_reject_connection(std::string& reason,
        const std::string& client_ip = "") const override;

private:
    std::shared_ptr<HttpFsm<pr::TcpConnection>> m_tcp_fsm;
    std::shared_ptr<HttpFsm<pr::SslConnection>> m_ssl_fsm;
    std::string doc_root_;
    ApiHandler api_handler_;   // 业务 API 路由（可空），工厂转发给每个 session
};

} // namespace pr::http

#endif // PR_FRAMEWORK_HTTP_HTTP_SERVER_H