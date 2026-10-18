#ifndef MAIL_SYSTEM_WEBMAIL_WEBMAIL_SERVER_H
#define MAIL_SYSTEM_WEBMAIL_WEBMAIL_SERVER_H

// ================================================================
// WebmailServer — 第四个协议前端（HTTP/JSON），与 mailServer 平级（kickoff §三）
//
// 复用 pr::http（HTTP/1.1）+ 引擎既有认证能力：业务编排薄、引擎复用厚。
// API 路由表见 api/api_router.h（health/login/logout/me，MVP 第一步）；
// 非 /api/* 一律落 doc_root 静态文件（前端 SPA，AI 生成）。
//
// 红线（kickoff 设计决策 4）：doc_root 永远不指到 mail 存储目录——
// 邮件体/附件只经带鉴权 handler 流式吐出（后续提交），静态管线碰不到存储。
// ================================================================

#include "framework/http/http_server.h"
#include "mail_system/back/webmail/api/api_router.h"
#include "mail_system/back/webmail/auth_service.h"
#include "mail_system/back/webmail/mail_service.h"
#include "mail_system/back/webmail/session_store.h"

#include <memory>

namespace mail_system::webmail {

class WebmailServer : public pr::http::HttpServer {
public:
    // auth_cache / sessions 缺省自建（多 server 共享登录态时从装配根传入）
    WebmailServer(const mail_system::ServerConfig& config,
                  std::string doc_root,
                  std::shared_ptr<pr::ThreadPoolBase> io_pool,
                  std::shared_ptr<pr::ThreadPoolBase> worker_pool,
                  std::shared_ptr<router::IShardRouter> shard_router,
                  std::shared_ptr<AuthCache> auth_cache = nullptr,
                  std::shared_ptr<SessionStore> sessions = nullptr,
                  std::shared_ptr<ISmtpSubmitter> submitter = nullptr);

    SessionStore& sessions() { return *sessions_; }

private:
    std::shared_ptr<AuthCache> auth_cache_;    // 保活（AuthService 内部按值持有拷贝）
    std::shared_ptr<SessionStore> sessions_;
    std::unique_ptr<AuthService> auth_service_;
    std::unique_ptr<MailService> mail_service_;
    std::shared_ptr<ISmtpSubmitter> submitter_;
    ApiRouter router_;
};

} // namespace mail_system::webmail

#endif // MAIL_SYSTEM_WEBMAIL_WEBMAIL_SERVER_H
