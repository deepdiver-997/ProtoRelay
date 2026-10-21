#ifndef PR_FRAMEWORK_STORAGE_DISTRIBUTED_FILE_STORAGE_PROVIDER_H
#define PR_FRAMEWORK_STORAGE_DISTRIBUTED_FILE_STORAGE_PROVIDER_H

#include "framework/storage/i_storage_provider.h"
#include "framework/storage/local_file_metadata_cache.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pr {
namespace storage {

class DistributedFileStorageProvider : public IStorageProvider, public LocalFileMetadataCache {
public:
    DistributedFileStorageProvider(std::vector<std::string> storage_roots,
                                   std::size_t replica_count = 1);

    bool ensure_ready(IoError& error) override;

    // Resolve a caller-defined relative object key using the same placement
    // rule as replicated IO. The full relative key is the hash input.
    std::string resolve_object_key(const std::string& relative_key) const;

    bool append_binary(const std::string& storage_key,
                       const char* data,
                       std::size_t size,
                       IoError& error) override;

    bool remove_object(const std::string& storage_key,
                       IoError& error) override;

    // 依次尝试各副本，任一可读即成功
    bool read_all(const std::string& storage_key,
                  std::string& out,
                  IoError& error) override;

    std::unique_ptr<pr::IDownloadStream> open_download(const std::string& key, IoError& error) override;

private:
    static std::string ensure_trailing_slash(const std::string& path);

    std::string strip_root_prefix(const std::string& storage_key) const;
    std::size_t get_primary_index(const std::string& relative_key) const;
    std::vector<std::size_t> pick_replica_indices(const std::string& relative_key) const;

    std::vector<std::string> roots_;
    std::size_t replica_count_;
};

} // namespace storage
} // namespace pr

#endif // PR_FRAMEWORK_STORAGE_DISTRIBUTED_FILE_STORAGE_PROVIDER_H
