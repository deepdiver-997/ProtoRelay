#pragma once
#include "mail_system/back/storage/i_storage_provider.h"
#include "mail_system/back/storage/mail_object_key_layout.h"
#include "framework/storage/hdfs_web_storage_provider.h"
#include <cstdint>
#include <string>
#include <utility>

namespace mail_system::storage {
// Application layout policy layered over generic object IO.
class HdfsWebStorageProvider : public pr::storage::HdfsWebStorageProvider, public IMailObjectKeyLayout {
public:
    using pr::storage::HdfsWebStorageProvider::HdfsWebStorageProvider;
    std::string build_mail_body_key(std::uint64_t id) override {
        return layout_.build_mail_body_key(id);
    }
    std::string build_attachment_key(std::uint64_t id, const std::string& filename) override {
        return layout_.build_attachment_key(id, filename);
    }
private:
    MailObjectKeyLayout layout_{MailObjectKeyLayout::Kind::relative};
};
} // namespace mail_system::storage
