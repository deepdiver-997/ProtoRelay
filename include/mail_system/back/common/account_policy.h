#pragma once
#include "framework/thread_pool/thread_pool_base.h"
#include "mail_system/back/common/account_authentication.h"
#include "framework/cache/lru_cache.h"
#include "mail_system/back/common/auth_cache.h"
#include "mail_system/back/algorithm/smtp_utils.h"
#include "mail_system/back/db/db_pool.h"
#include "mail_system/back/router/i_shard_router.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mail_system {

class AccountPolicy {
  public:
    AccountPolicy(std::shared_ptr<router::IShardRouter> shard_router, std::shared_ptr<AuthCache> auth_cache,
                  std::shared_ptr<pr::ThreadPoolBase> worker_pool,
                  std::shared_ptr<IAccountAuthenticator> authenticator = nullptr)
        : router_(std::move(shard_router)), cache_(std::move(auth_cache)),
          worker_pool_(std::move(worker_pool)),
          authenticator_(authenticator ? std::move(authenticator) :
              std::make_shared<AccountAuthenticationService>(router_, cache_, worker_pool_)) {}

    // SMTP retains the existing short AuthCache window. Web login requests fresh=true
    // and receives the post-rehash credential stamp used by durable sessions.
    void login_async(const std::string &mail_address, const std::string &password,
                     std::function<void(AccountAuthResult)> cb, bool fresh = false);

    void set_authenticator(std::shared_ptr<IAccountAuthenticator> authenticator) {
        if (authenticator) authenticator_ = std::move(authenticator);
    }

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

    // ===== 本域发件人边界（防伪造，2026-10-20）=====
    // 合法本域邮件只有三种入口：认证提交（465/587）、webmail MailSubmissionService
    // （进程内直提，不走 SMTP）、trusted_relay_networks 节点间中继。因此"既未认证
    // 又不在信任网段"的公网 25 直连会话出现本域发件人只能是伪造。判定逻辑全部
    // 收在此处静态纯函数（无网络、无 DB），便于单测；域名比较一律大小写不敏感。

    // 信封发件人（MAIL FROM 尖括号内内容）域名是否本域；空信封 <>（退信）恒为 false。
    // 尖括号去除与 InboundVerifier::extract_domain 同口径（兜底，正常入参不带括号）。
    static bool envelope_sender_is_local(const std::string &sender, const std::string &system_domain) {
        auto at = sender.rfind('@');
        if (at == std::string::npos) return false;   // 空信封 / 无域：不判本域
        auto domain = algorithm::trim(sender.substr(at + 1));
        if (!domain.empty() && domain.back() == '>') domain.pop_back();
        if (!domain.empty() && domain.front() == '<') domain.erase(0, 1);
        return algorithm::to_lower(domain) == algorithm::to_lower(system_domain);
    }

    // From 头值（"user@d" / "Name <user@d>"）的域名（小写）；无 @ 视为无法解析，返回空。
    // 入站校验的 InboundVerifier::extract_from_header_domain 复用本实现，避免两处解析漂移。
    static std::string from_header_domain(const std::string &from_value) {
        return from_header_address_domain(from_value).second;
    }

    // From 头值的 addr-spec（去显示名/尖括号）；无 @ 视为无法解析，返回空。
    static std::string from_header_address(const std::string &from_value) {
        return from_header_address_domain(from_value).first;
    }

    // From 头域名是否本域；无法解析（空域名）→ false（不因本规则拒收）。
    static bool from_header_is_local(const std::string &from_value, const std::string &system_domain) {
        const auto domain = from_header_domain(from_value);
        return !domain.empty() && domain == algorithm::to_lower(system_domain);
    }

    // 认证提交的 From 头地址与认证账号地址精确相等（大小写不敏感；只做精确匹配，
    // 不处理别名/子地址）。任一侧无法解析 → false。
    static bool from_header_matches_account(const std::string &from_value, const std::string &authenticated) {
        const auto addr = from_header_address(from_value);
        return !addr.empty() && !authenticated.empty() &&
               algorithm::to_lower(addr) == algorithm::to_lower(algorithm::trim(authenticated));
    }

    // 规则 3 别名分支的集合判定：登录账号 ∈ From 头地址的别名目标集合
    // （目标来自 AccountPolicy::alias_targets_async / AliasCache），大小写不敏感。
    // 空集合（非别名 / DB 不可用 / 查询失败）→ false = 不匹配（调用方 fail-closed）。
    static bool account_in_alias_targets(const std::vector<std::string> &targets,
                                         const std::string &authenticated) {
        const auto who = algorithm::to_lower(algorithm::trim(authenticated));
        if (who.empty()) return false;
        for (const auto &t : targets)
            if (algorithm::to_lower(algorithm::trim(t)) == who) return true;
        return false;
    }

  private:
    // From 头值 → {addr-spec, 小写域名}。"Name <user@d>" 取尖括号内；否则整值。
    // 域名小写，addr-spec 保持原样（由调用方决定比较口径）。
    static std::pair<std::string, std::string> from_header_address_domain(const std::string &from_value) {
        auto begin = from_value.find('<');
        auto end = from_value.find('>');
        std::string addr;
        if (begin != std::string::npos && end != std::string::npos) {
            addr = from_value.substr(begin + 1, end - begin - 1);
        } else {
            addr = from_value;
        }
        auto at = addr.find('@');
        if (at == std::string::npos) return {};
        return {algorithm::trim(addr), algorithm::to_lower(addr.substr(at + 1))};
    }

    std::shared_ptr<AuthCache> recipients_ = std::make_shared<AuthCache>();
    std::shared_ptr<AliasCache> aliases_ = std::make_shared<AliasCache>(10000, std::chrono::seconds(300));
    std::shared_ptr<router::IShardRouter> router_;
    std::shared_ptr<AuthCache> cache_;
    std::shared_ptr<pr::ThreadPoolBase> worker_pool_;
    std::shared_ptr<IAccountAuthenticator> authenticator_;
};

} // namespace mail_system
