#pragma once
#include "mail_system/back/storage/i_storage_provider.h"
#include "mail_system/back/storage/mail_object_key_layout.h"
#include "framework/storage/async_storage_provider.h"
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace mail_system::storage {
class AsyncStorageProvider : public pr::storage::AsyncStorageProvider, public IMailObjectKeyLayout {
public:
    using Executor = pr::storage::AsyncStorageProvider::Executor;
    AsyncStorageProvider(std::shared_ptr<IStorageProvider> inner, Executor executor)
        : pr::storage::AsyncStorageProvider(inner, std::move(executor)), layout_provider_(std::move(inner)) {}
    // The framework base forwards IO and IObjectMetadataCache. This application
    // decorator forwards the inner mail capability, sharing its sequence state.
    std::string build_mail_body_key(std::uint64_t id) override {
        return mail_system::storage::build_mail_body_key(layout_provider_, id);
    }
    std::string build_attachment_key(std::uint64_t id, const std::string& filename) override {
        return mail_system::storage::build_attachment_key(layout_provider_, id, filename);
    }
private:
    std::shared_ptr<IStorageProvider> layout_provider_;
};
} // namespace mail_system::storage
