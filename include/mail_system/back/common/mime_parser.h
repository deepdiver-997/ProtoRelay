#pragma once
// Compatibility entry points for callers with an explicit local body path.
// Pure parsing and JSON codecs are available from mime_tree.h without file IO.
#include "mail_system/back/common/mime_metadata_store.h"
#include "framework/storage/local_file_metadata_cache.h"
#include "framework/storage/i_storage_provider.h"

namespace mail_system {
inline CachedMimeMetadataStore local_mime_metadata_store() {
    static auto cache = std::make_shared<pr::storage::LocalFileMetadataCache>();
    return CachedMimeMetadataStore(cache);
}
inline CachedMimeMetadataStore mime_metadata_store_for(
    const std::shared_ptr<pr::storage::IStorageProvider>& provider) {
    // Only absence of a provider enables the legacy local-path adapter.
    if (!provider) return local_mime_metadata_store();
    return CachedMimeMetadataStore(std::dynamic_pointer_cast<pr::storage::IObjectMetadataCache>(provider));
}
inline bool save_mime_tree(const std::string& path, const MimePart& root) {
    auto store = local_mime_metadata_store();
    return save_mime_tree(store, path, root);
}
inline bool load_mime_tree(const std::string& path, MimePart& root) {
    auto store = local_mime_metadata_store();
    return load_mime_tree(store, path, root);
}
inline bool ensure_mime_tree(const std::string& path, std::string_view raw, MimePart& root) {
    auto store = local_mime_metadata_store();
    return ensure_mime_tree(store, path, raw, root);
}
}
