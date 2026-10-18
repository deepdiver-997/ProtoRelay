#pragma once
#include "framework/thread_pool/thread_pool_base.h"
#include "framework/cache/lru_cache.h"
#include "mail_system/back/common/credential_stamp.h"
#include "mail_system/back/common/auth_cache.h"
#include "mail_system/back/common/password_rehash.h"
#include "mail_system/back/common/safe_parse.h"
#include "mail_system/back/db/db_pool.h"
#include "mail_system/back/db/sql_queries.h"
#include "mail_system/back/router/i_shard_router.h"

#include <functional>
#include <memory>
#include <string>

namespace mail_system {

struct AccountAuthResult {
    bool ok = false;
    // true → 503（DB 不可用，可重试）；false 且 !ok → 401（凭据错/账号禁用，
    // 与三协议一样不区分原因，避免给未认证调用方提供账号枚举通道）。
    bool server_error = false;
    uint64_t user_id = 0;
    std::string address;
    std::string credential_stamp;
    int shard = 0;
};

class AccountPolicy {
  public:
    AccountPolicy(std::shared_ptr<router::IShardRouter> shard_router, std::shared_ptr<AuthCache> auth_cache,
                  std::shared_ptr<pr::ThreadPoolBase> worker_pool)
        : router_(std::move(shard_router)), cache_(std::move(auth_cache)),
          worker_pool_(std::move(worker_pool)) {}

    // SMTP retains the existing short AuthCache window. Web login requests fresh=true
    // and receives the post-rehash credential stamp used by durable sessions.
    void login_async(const std::string &mail_address, const std::string &password,
                     std::function<void(AccountAuthResult)> cb, bool fresh = false);

    using AliasCache = pr::LruCache<std::string, std::vector<std::string>>;
    void set_recipient_caches(std::shared_ptr<AuthCache> recipients, std::shared_ptr<AliasCache> aliases) {
        recipients_ = std::move(recipients);
        aliases_ = std::move(aliases);
    }
    void user_exists_async(const std::string &address, std::function<void(bool)> cb);
    void alias_targets_async(const std::string &address, std::function<void(std::vector<std::string>)> cb);
    void check_send_quota_async(const std::string &address, int daily_limit, std::function<void(bool)> cb);
    static std::string submission_sender(const std::string &authenticated, const std::string &requested) {
        return authenticated.empty() ? requested : authenticated;
    }
    static bool recipient_allowed(const std::string &address, const std::string &domain, bool external);

  private:
    std::shared_ptr<AuthCache> recipients_ = std::make_shared<AuthCache>();
    std::shared_ptr<AliasCache> aliases_ = std::make_shared<AliasCache>(10000, std::chrono::seconds(300));
    std::shared_ptr<router::IShardRouter> router_;
    std::shared_ptr<AuthCache> cache_;
    std::shared_ptr<pr::ThreadPoolBase> worker_pool_;
};

} // namespace mail_system
