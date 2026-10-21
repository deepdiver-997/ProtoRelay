#pragma once
#include "framework/storage/i_storage_provider.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace mail_system::storage {
// Optional application capability. Generic IO providers need not know anything
// about mail IDs, attachment names, counters or persisted object layouts.
class IMailObjectKeyLayout {
public:
    virtual ~IMailObjectKeyLayout() = default;
    virtual std::string build_mail_body_key(std::uint64_t mail_id) = 0;
    virtual std::string build_attachment_key(std::uint64_t mail_id,
                                              const std::string& original_filename) = 0;
};
// One instance belongs to one concrete application provider. Decorators forward
// that same instance instead of restarting its attachment sequence.
class MailObjectKeyLayout final : public IMailObjectKeyLayout {
public:
    enum class Kind { local, relative, s3, discard };
    explicit MailObjectKeyLayout(Kind kind, std::string mail_root = {}, std::string attachment_root = {});
    std::string build_mail_body_key(std::uint64_t mail_id) override;
    std::string build_attachment_key(std::uint64_t mail_id, const std::string& original_filename) override;
private:
    Kind kind_;
    std::string mail_root_, attachment_root_;
    std::atomic<std::uint64_t> attachment_seq_{0};
};
// The application local adapter uses the same root spelling for key layout
// and directory preparation, including the historical trailing-backslash rule.
std::string normalize_mail_storage_root(std::string root);

// Missing capability is a configuration error, never permission to invent a
// local path for an opaque provider. Null pointers fail explicitly here too.
std::string build_mail_body_key(pr::storage::IStorageProvider& provider, std::uint64_t mail_id);
std::string build_attachment_key(pr::storage::IStorageProvider& provider, std::uint64_t mail_id,
                                  const std::string& original_filename);
std::string build_mail_body_key(const std::shared_ptr<pr::storage::IStorageProvider>& provider,
                                 std::uint64_t mail_id);
std::string build_attachment_key(const std::shared_ptr<pr::storage::IStorageProvider>& provider,
                                  std::uint64_t mail_id, const std::string& original_filename);
} // namespace mail_system::storage
