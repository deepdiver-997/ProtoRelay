#pragma once
#include "mail_system/back/common/mime_tree.h"
#include "framework/storage/i_object_metadata_cache.h"
#include <memory>

namespace mail_system {
class IMimeMetadataStore {
public:
    virtual ~IMimeMetadataStore() = default;
    virtual bool load(const std::string& object_key, MimePart& root) = 0;
    virtual bool save(const std::string& object_key, const MimePart& root) = 0;
};

// No filesystem assumptions: absent capability means a cache miss, including
// object providers that intentionally avoid synchronous remote metadata IO.
class CachedMimeMetadataStore final : public IMimeMetadataStore {
public:
    explicit CachedMimeMetadataStore(std::shared_ptr<pr::storage::IObjectMetadataCache> cache)
        : cache_(std::move(cache)) {}
    bool load(const std::string& key, MimePart& root) override {
        root = MimePart{};
        if (!cache_ || key.empty()) return false;
        try {
            std::string value;
            return cache_->load_metadata(key, "mime", value) && deserialize_mime_tree(value, root);
        } catch (...) { root = MimePart{}; return false; }
    }
    bool save(const std::string& key, const MimePart& root) override {
        if (!cache_ || key.empty() || root.type.empty()) return false;
        try { return cache_->save_metadata(key, "mime", serialize_mime_tree(root)); }
        catch (...) { return false; }
    }
private:
    std::shared_ptr<pr::storage::IObjectMetadataCache> cache_;
};

inline bool load_mime_tree(IMimeMetadataStore& store, const std::string& key, MimePart& root) {
    root = MimePart{};
    try { return store.load(key, root); }
    catch (...) { root = MimePart{}; return false; }
}
inline bool save_mime_tree(IMimeMetadataStore& store, const std::string& key, const MimePart& root) {
    try { return store.save(key, root); } catch (...) { return false; }
}
inline bool ensure_mime_tree(IMimeMetadataStore& store, const std::string& key,
                             std::string_view raw, MimePart& root) {
    if (load_mime_tree(store, key, root) && root.metadata_version >= kMimeMetadataVersion) return true;
    root = MimePart{};
    if (raw.empty()) return false;
    parse_mime_tree(raw, root);
    if (root.type.empty()) return false;
    save_mime_tree(store, key, root); // cache failures never reject a readable message
    return true;
}
}
