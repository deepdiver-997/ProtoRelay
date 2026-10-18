#pragma once
#include "mail_system/back/router/i_shard_router.h"
#include "framework/thread_pool/thread_pool_base.h"
#include "framework/storage/i_download_stream.h"
#include <functional>
#include <optional>

namespace mail_system::webmail {
struct MailActionResult { bool server_error = false; bool not_found = false; };
struct AttachmentDownload : MailActionResult {
    std::string filename;
    std::shared_ptr<pr::IDownloadStream> source;
};
class MailActions {
public:
    MailActions(std::shared_ptr<router::IShardRouter> router, std::shared_ptr<pr::ThreadPoolBase> workers)
        : router_(std::move(router)), workers_(std::move(workers)) {}
    void update_async(uint64_t user, std::string address, uint64_t box, uint64_t mail,
                      std::optional<bool> read, std::optional<bool> starred, std::function<void(MailActionResult)> cb);
    void download_async(uint64_t user, std::string address, uint64_t box, uint64_t mail, uint64_t attachment,
                        std::function<void(AttachmentDownload)> cb);
private:
    std::shared_ptr<router::IShardRouter> router_;
    std::shared_ptr<pr::ThreadPoolBase> workers_;
};
}
