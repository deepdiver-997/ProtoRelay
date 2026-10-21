#ifndef PR_FRAMEWORK_STORAGE_NULL_STORAGE_PROVIDER_H
#define PR_FRAMEWORK_STORAGE_NULL_STORAGE_PROVIDER_H

#include "framework/storage/i_storage_provider.h"
#include <string>

namespace pr {
namespace storage {

// 空存储实现 —— 所有数据直接丢弃，不写盘。
// 用于基准测试 / 压测，避免大量邮件文件磨损 SSD。
class NullStorageProvider : public IStorageProvider {
public:
    NullStorageProvider() = default;

    bool ensure_ready(IoError& error) override {
        return true;
    }

    bool append_binary(const std::string&, const char*, std::size_t size,
                       IoError& error) override {
        // 假装写入成功
        return true;
    }

    bool remove_object(const std::string&, IoError& error) override {
        return true;
    }

    // 写入是假装成功的，所以什么都读不回来。明确失败，不返回空内容冒充成功 ——
    // 否则上层会把「读到空邮件」当成正常结果。
    bool read_all(const std::string&, std::string&, IoError& error) override {
        error = IoError::permanent("null storage provider discards writes; nothing can be read back");
        return false;
    }
};

} // namespace storage
} // namespace pr

#endif // PR_FRAMEWORK_STORAGE_NULL_STORAGE_PROVIDER_H
