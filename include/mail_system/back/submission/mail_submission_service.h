#pragma once
#include "mail_system/back/common/account_policy.h"
#include "mail_system/back/server_config.h"
#include "framework/net/executor.h"
#include "mail_system/back/algorithm/snow.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mail_system::persist_storage { class PersistentQueue; }

namespace mail_system::submission {
using ExecutorProvider = std::function<std::shared_ptr<pr::net::IExecutor>()>;
struct Request {
    uint64_t user_id = 0;
    std::string sender, subject, message_id, raw;
    std::vector<std::string> recipients;
};
enum class Status { Accepted, AuthFailed, Rejected, Unavailable, Unknown };
struct Result {
    Status status = Status::Unavailable;
    int code = 0;
    std::string stage;
};
// Trusted in-process entry point. Callers supply an authenticated actor, never an HTTP From.
// Storage and metadata use the same PersistentQueue contract as SMTP after_persist.
class MailSubmissionService : public std::enable_shared_from_this<MailSubmissionService> {
  public:
    using Complete = std::function<void(Result)>;
    MailSubmissionService(std::shared_ptr<router::IShardRouter> router,
                          std::shared_ptr<pr::ThreadPoolBase> workers, std::shared_ptr<AccountPolicy> policy,
                          std::shared_ptr<persist_storage::PersistentQueue> queue,
                          ExecutorProvider executors, ServerConfig config, uint16_t node_id = 1,
                          std::shared_ptr<repository::IAccountRepository> accounts = nullptr);
    void submit_async(Request request, Complete cb);

  private:
    struct Operation;
    std::shared_ptr<router::IShardRouter> router_;
    std::shared_ptr<pr::ThreadPoolBase> workers_;
    std::shared_ptr<repository::IAccountRepository> accounts_;
    std::shared_ptr<AccountPolicy> policy_;
    std::shared_ptr<persist_storage::PersistentQueue> queue_;
    ExecutorProvider executors_;
    ServerConfig config_;
    algorithm::SnowflakeGenerator ids_;
};
} // namespace mail_system::submission
