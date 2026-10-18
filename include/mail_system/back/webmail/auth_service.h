#pragma once
#include "mail_system/back/common/account_policy.h"
namespace mail_system::webmail {
using AuthOutcome = AccountAuthResult;
class AuthService {
public:
    AuthService(std::shared_ptr<router::IShardRouter> router, std::shared_ptr<AuthCache> cache,
                std::shared_ptr<pr::ThreadPoolBase> workers, bool fresh = false)
        : policy_(std::make_shared<AccountPolicy>(std::move(router), std::move(cache), std::move(workers))), fresh_(fresh) {}
    void login_async(const std::string& address, const std::string& password, std::function<void(AuthOutcome)> cb) {
        policy_->login_async(address, password, std::move(cb), fresh_);
    }
private:
    std::shared_ptr<AccountPolicy> policy_;
    bool fresh_;
};
} // namespace mail_system::webmail
