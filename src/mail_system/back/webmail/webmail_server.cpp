#include "mail_system/back/webmail/webmail_server.h"

#include <utility>

namespace mail_system::webmail {

WebmailServer::WebmailServer(const mail_system::ServerConfig& config,
                             std::string doc_root,
                             std::shared_ptr<pr::ThreadPoolBase> io_pool,
                             std::shared_ptr<pr::ThreadPoolBase> worker_pool,
                             std::shared_ptr<router::IShardRouter> shard_router,
                             std::shared_ptr<AuthCache> auth_cache,
                             std::shared_ptr<SessionStore> sessions)
    : pr::http::HttpServer(config, std::move(doc_root), io_pool, worker_pool,
                           std::move(shard_router)),
      auth_cache_(auth_cache ? std::move(auth_cache) : std::make_shared<AuthCache>()),
      sessions_(sessions ? std::move(sessions) : std::make_shared<SessionStore>()),
      auth_service_(std::make_unique<AuthService>(
          get_shard_router(), auth_cache_, worker_pool)),
      router_(*auth_service_, *sessions_) {
    // 工厂把 handler 注入每个新连接的 session；须在 start() 之前设置完成
    set_api_handler([this](const pr::http::HttpRequest& req,
                           std::function<void(pr::http::HttpResponse)> complete) {
        return router_.handle(req, std::move(complete));
    });
}

} // namespace mail_system::webmail
