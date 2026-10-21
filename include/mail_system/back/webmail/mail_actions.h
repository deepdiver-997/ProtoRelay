#pragma once
#include "mail_system/back/repository/webmail_action_repository.h"
#include "mail_system/back/router/i_shard_router.h"
#include "framework/thread_pool/thread_pool_base.h"
#include "framework/storage/i_download_stream.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <optional>

namespace mail_system::webmail {
struct AttachmentDownload : MailActionResult {
    std::string filename;
    std::shared_ptr<pr::IDownloadStream> source;
};
class MailActions {
public:
    MailActions(std::shared_ptr<router::IShardRouter> router, std::shared_ptr<pr::ThreadPoolBase> workers,
                std::shared_ptr<repository::IWebmailActionRepository> persistence = nullptr)
        : router_(std::move(router)), workers_(std::move(workers)),
          repository_(persistence ? std::move(persistence)
              : repository::make_sql_webmail_action_repository(router_)) {}
    void update_async(uint64_t user, std::string address, uint64_t box, uint64_t mail,
                      std::optional<bool> read, std::optional<bool> starred, std::function<void(MailActionResult)> cb);
    void download_async(uint64_t user, std::string address, uint64_t box, uint64_t mail, uint64_t attachment,
                        std::function<void(AttachmentDownload)> cb);
private:
    std::shared_ptr<router::IShardRouter> router_;
    std::shared_ptr<pr::ThreadPoolBase> workers_;
    std::shared_ptr<repository::IWebmailActionRepository> repository_;
};
}
