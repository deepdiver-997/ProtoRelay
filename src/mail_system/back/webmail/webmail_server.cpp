#include "mail_system/back/mailServer/outbound_server.h"
#include "mail_system/back/submission/outbound_config_factory.h"
#include "mail_system/back/webmail/webmail_server.h"

#include "framework/net/executor.h"
#include "mail_system/back/webmail/direct_submitter.h"
#include "mail_system/back/mailServer/outbound/cares_dns_resolver.h"
#include "mail_system/back/mailServer/outbound/outbound_config.h"
#include <utility>

namespace mail_system::webmail {

WebmailServer::WebmailServer(const mail_system::ServerConfig& config,
                             std::string doc_root,
                             std::shared_ptr<pr::ThreadPoolBase> io_pool,
                             std::shared_ptr<pr::ThreadPoolBase> worker_pool,
                             std::shared_ptr<router::IShardRouter> shard_router,
                             std::shared_ptr<AuthCache> auth_cache,
                             std::shared_ptr<SessionStore> sessions,
                             std::shared_ptr<ISmtpSubmitter> submitter, uint16_t submission_node_id)
    : pr::http::BasicHttpServer<MailServerBase>(config, std::move(doc_root), io_pool, worker_pool,
                           std::move(shard_router)),
      auth_cache_(auth_cache ? std::move(auth_cache) : std::make_shared<AuthCache>()),
      sessions_(sessions ? std::move(sessions) : (config.use_database
          ? std::make_shared<SessionStore>(get_shard_router(), worker_pool)
          : std::make_shared<SessionStore>())),
      auth_service_(std::make_shared<AuthService>(
          get_shard_router(), auth_cache_, worker_pool, config.use_database)),
      mail_service_(std::make_shared<MailService>(get_shard_router())),
      mail_actions_(std::make_shared<MailActions>(get_shard_router(), worker_pool)),
      submitter_(std::move(submitter)) {
    if (!submitter_ && config.use_database) {
        if (submission_node_id > 1023) throw std::invalid_argument("webmail_node_id must be 0..1023");
        auto cfg = std::atomic_load(&m_config);
        m_dnsResolver = std::make_shared<outbound::CaresDnsResolver>();
        std::static_pointer_cast<outbound::CaresDnsResolver>(m_dnsResolver)->set_metrics(get_metrics());
        persist_queue_ = std::make_shared<persist_storage::PersistentQueue>(get_shard_router(), worker_pool);
        persist_queue_->set_local_domain(config.system_domain);
        persist_storage::PersistentQueuePressureConfig pressure;
        pressure.max_inflight_mails = config.persist_max_inflight_mails;
        pressure.min_available_memory_mb = config.persist_min_available_memory_mb;
        pressure.min_db_available_connections = config.persist_min_db_available_connections;
        persist_queue_->set_pressure_config(pressure);
        persist_queue_->set_max_attempts(config.outbound_max_attempts);
        persist_queue_->inject_metrics(get_metrics());
        outbound_ = std::make_shared<outbound::OutboundServer>(this);
        outbound_->set_metrics(get_metrics());
        outbound_->set_config(submission::outbound_config(*cfg));
        persist_queue_->set_outbound_server(outbound_);
        auto service = std::make_shared<submission::MailSubmissionService>(get_shard_router(), worker_pool,
            std::make_shared<AccountPolicy>(get_shard_router(), auth_cache_, worker_pool), persist_queue_,
            [pool = std::dynamic_pointer_cast<pr::net::IExecutorProvider>(io_pool)] {
                if (!pool) throw std::invalid_argument("submission runtime must provide executors");
                return pool->next_executor();
            },
            config, submission_node_id);
        submitter_ = std::make_shared<DirectSubmitter>(std::move(service));
        outbound_->start();
    }
    // 工厂把 handler 注入每个新连接的 session；须在 start() 之前设置完成
    router_ = std::make_shared<ApiRouter>(*auth_service_, *sessions_, *mail_service_, submitter_.get(), mail_actions_.get());
    set_api_handler([router = router_, auth = auth_service_, sessions = sessions_,
                     mail = mail_service_, actions = mail_actions_, submitter = submitter_](const pr::http::HttpRequest& req,
                           std::function<void(pr::http::HttpResponse)> complete) {
        return router->handle(req, std::move(complete));
    });
}

WebmailServer::~WebmailServer() { stop(); }
void WebmailServer::stop(pr::ServerState state) {
    if (outbound_) outbound_->stop();
    if (persist_queue_) persist_queue_->shutdown();
    pr::http::BasicHttpServer<MailServerBase>::stop(state);
}
} // namespace mail_system::webmail
