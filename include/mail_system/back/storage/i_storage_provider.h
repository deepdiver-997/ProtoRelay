#ifndef MAIL_SYSTEM_STORAGE_I_STORAGE_PROVIDER_H
#define MAIL_SYSTEM_STORAGE_I_STORAGE_PROVIDER_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace mail_system {
namespace storage {

class IStorageProvider {
public:
    virtual ~IStorageProvider() = default;

    virtual bool ensure_ready(std::string& error) = 0;

    virtual std::string build_mail_body_key(std::uint64_t mail_id) = 0;

    virtual std::string build_attachment_key(std::uint64_t mail_id,
                                             const std::string& original_filename) = 0;

    virtual bool append_binary(const std::string& storage_key,
                               const char* data,
                               std::size_t size,
                               std::string& error) = 0;

    virtual bool remove_object(const std::string& storage_key,
                               std::string& error) = 0;
};

} // namespace storage
} // namespace mail_system

#endif // MAIL_SYSTEM_STORAGE_I_STORAGE_PROVIDER_H
