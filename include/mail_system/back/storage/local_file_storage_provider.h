#pragma once
#include "mail_system/back/storage/i_storage_provider.h"
#include "mail_system/back/storage/mail_object_key_layout.h"
#include "framework/storage/local_file_storage_provider.h"
#include <cstdint>
#include <string>
#include <utility>

namespace mail_system::storage {
// Application layout policy layered over generic object IO.
class LocalFileStorageProvider : public pr::storage::LocalFileStorageProvider, public IMailObjectKeyLayout {
public:
    LocalFileStorageProvider(std::string mail_root, std::string attachment_root)
        : pr::storage::LocalFileStorageProvider({normalize_mail_storage_root(mail_root), normalize_mail_storage_root(attachment_root)}),
          layout_(MailObjectKeyLayout::Kind::local, std::move(mail_root), std::move(attachment_root)) {}
    std::string build_mail_body_key(std::uint64_t id) override {
        return layout_.build_mail_body_key(id);
    }
    std::string build_attachment_key(std::uint64_t id, const std::string& filename) override {
        return layout_.build_attachment_key(id, filename);
    }
private:
    MailObjectKeyLayout layout_{MailObjectKeyLayout::Kind::local};
};
} // namespace mail_system::storage
