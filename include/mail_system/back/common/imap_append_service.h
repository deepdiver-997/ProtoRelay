#pragma once
#include "framework/storage/i_storage_provider.h"
#include "framework/thread_pool/thread_pool_base.h"
#include "mail_system/back/repository/imap_message_repository.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace mail_system {
struct ImapAppendRequest {
    uint64_t user_id = 0, mailbox_id = 0;
    std::string subject, body, authenticated_address;
    int initial_status = 0;
};
struct ImapAppendResult {
    bool ok = false;
    uint64_t mail_id = 0;
    std::string body_key, address, error;
    bool cleanup_complete = true;
};
class IImapAppendService {
public:
    using Lease = repository::IImapMessageRepository::Lease;
    using Complete = std::function<void(ImapAppendResult)>;
    virtual ~IImapAppendService() = default;
    // Body IO and compensation run on the injected Worker. The lease and its
    // pool must remain valid across pending DB work. Completion may run on a
    // Worker/DB executor; the adapter must resume protocol state on its owner.
    virtual void append_async(Lease, std::shared_ptr<pr::storage::IStorageProvider>,
                              ImapAppendRequest, Complete) = 0;
};
std::shared_ptr<IImapAppendService> make_imap_append_service(
    std::shared_ptr<pr::ThreadPoolBase> worker,
    std::shared_ptr<repository::IImapMessageRepository> messages = {});
} // namespace mail_system
