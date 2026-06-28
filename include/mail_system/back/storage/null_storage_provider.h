#ifndef MAIL_SYSTEM_STORAGE_NULL_STORAGE_PROVIDER_H
#define MAIL_SYSTEM_STORAGE_NULL_STORAGE_PROVIDER_H

#include "mail_system/back/storage/i_storage_provider.h"
#include <string>

namespace mail_system {
namespace storage {

// 空存储实现 —— 所有数据直接丢弃，不写盘。
// 用于基准测试 / 压测，避免大量邮件文件磨损 SSD。
class NullStorageProvider : public IStorageProvider {
public:
    NullStorageProvider() = default;

    bool ensure_ready(std::string& error) override {
        return true;
    }

    std::string build_mail_body_key(std::uint64_t mail_id) override {
        return "/dev/null/" + std::to_string(mail_id);
    }

    std::string build_attachment_key(std::uint64_t mail_id,
                                     const std::string& original_filename) override {
        return "/dev/null/" + std::to_string(mail_id) + "_" + original_filename;
    }

    bool append_binary(const std::string&, const char*, std::size_t size,
                       std::string& error) override {
        // 假装写入成功
        return true;
    }

    bool remove_object(const std::string&, std::string& error) override {
        return true;
    }
};

} // namespace storage
} // namespace mail_system

#endif // MAIL_SYSTEM_STORAGE_NULL_STORAGE_PROVIDER_H
