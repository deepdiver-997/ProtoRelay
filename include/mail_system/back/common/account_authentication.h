#pragma once

#include "framework/thread_pool/thread_pool_base.h"
#include "mail_system/back/common/auth_cache.h"
#include "mail_system/back/repository/account_repository.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

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

// Authentication is shared by all mail frontends. Protocols own their reply,
// retry limit and session/token policies; this service owns credential lookup,
// cache checks, legacy rehash and last_login ordering.
class IAccountAuthenticator {
public:
    using Complete = std::function<void(AccountAuthResult)>;
    virtual ~IAccountAuthenticator() = default;
    virtual void login_async(const std::string& address, const std::string& password,
                             Complete complete, bool fresh = false) = 0;
};

class AccountAuthenticationService final : public IAccountAuthenticator {
public:
    AccountAuthenticationService(std::shared_ptr<router::IShardRouter> router,
                                 std::shared_ptr<AuthCache> cache,
                                 std::shared_ptr<pr::ThreadPoolBase> workers,
                                 std::shared_ptr<repository::IAccountRepository> accounts = nullptr)
        : accounts_(accounts ? std::move(accounts) : repository::make_account_repository(std::move(router))),
          cache_(std::move(cache)), workers_(std::move(workers)) {}
    // Completion may be inline without workers. With workers, credential hashing
    // is dispatched there even when a database completes on an IO thread.
    // Each asynchronous chain retains its connection and pool until completion.
    void login_async(const std::string& address, const std::string& password,
                     Complete complete, bool fresh = false) override;
private:
    std::shared_ptr<repository::IAccountRepository> accounts_;
    std::shared_ptr<AuthCache> cache_;
    std::shared_ptr<pr::ThreadPoolBase> workers_;
};
} // namespace mail_system
