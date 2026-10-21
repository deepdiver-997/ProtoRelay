#pragma once
#include "mail_system/back/storage/i_storage_provider.h"
#include "mail_system/back/storage/mail_object_key_layout.h"
#include "framework/storage/distributed_file_storage_provider.h"
#include <cstdint>
#include <string>
#include <utility>

namespace mail_system::storage {
// Application layout policy layered over generic object IO.
class DistributedFileStorageProvider : public pr::storage::DistributedFileStorageProvider, public IMailObjectKeyLayout {
public:
    using pr::storage::DistributedFileStorageProvider::DistributedFileStorageProvider;
    std::string build_mail_body_key(std::uint64_t id) override {
        return resolve_object_key(layout_.build_mail_body_key(id));
    }
    std::string build_attachment_key(std::uint64_t id, const std::string& filename) override {
        return resolve_object_key(layout_.build_attachment_key(id, filename));
    }
private:
    MailObjectKeyLayout layout_{MailObjectKeyLayout::Kind::relative};
};
} // namespace mail_system::storage
