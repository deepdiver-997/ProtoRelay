#pragma once
#include "mail_system/back/entities/webmail_persistence.h"
#include <chrono>
#include <memory>
#include <string>

namespace mail_system::router { class IShardRouter; }
namespace mail_system::repository {
// Synchronous Worker-facing registry. Implementations store only token hashes
// and account stamps, enforce one live token per account, and revalidate fresh
// account credentials for every lookup. An absent token differs from DB failure.
class IWebSessionRepository {
public:
    virtual ~IWebSessionRepository() = default;
    virtual bool put(const std::string& token, const webmail::WebSessionInfo& user,
                     std::chrono::seconds ttl) = 0;
    virtual webmail::WebSessionLookup lookup(const std::string& token) = 0;
    virtual bool erase(const std::string& token) = 0;
};
std::shared_ptr<IWebSessionRepository> make_sql_web_session_repository(
    std::shared_ptr<router::IShardRouter> router);
} // namespace mail_system::repository
