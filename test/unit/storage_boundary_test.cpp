#undef NDEBUG
#include <cassert>
#include "mail_system/back/common/mime_parser.h"
#include "framework/storage/async_storage_provider.h"
#include "framework/storage/null_storage_provider.h"
#include <filesystem>
#include <map>
#include <stdexcept>
#include <thread>

namespace {
const std::string raw = "Content-Type: text/plain; charset=utf-8\r\n\r\nhello\r\n";
class MemoryCache : public pr::storage::IObjectMetadataCache {
public:
    bool load_metadata(const std::string& key, const std::string& name, std::string& out) override {
        auto found = values.find(key + ":" + name);
        if (found == values.end()) return false;
        out = found->second;
        return true;
    }
    bool save_metadata(const std::string& key, const std::string& name,
                       const std::string& value) override {
        values[key + ":" + name] = value;
        return true;
    }
    std::map<std::string, std::string> values;
};
class FailingStore : public mail_system::IMimeMetadataStore {
public:
    bool load(const std::string&, MimePart&) override { throw std::runtime_error("cache offline"); }
    bool save(const std::string&, const MimePart&) override { throw std::runtime_error("cache offline"); }
};
class CacheProvider : public pr::storage::NullStorageProvider, public MemoryCache {};
class TempDirectory {
public:
    TempDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "storage_boundary_XXXXXX").string();
        auto created = ::mkdtemp(pattern.data());
        assert(created);
        path = created;
    }
    ~TempDirectory() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    std::filesystem::path path;
};
}

int main() {
    using namespace mail_system;
    auto cache = std::make_shared<MemoryCache>();
    CachedMimeMetadataStore store(cache);
    MimePart tree;
    assert(ensure_mime_tree(store, "opaque:key/123", raw, tree));
    assert(tree.type == "text" && tree.metadata_version == kMimeMetadataVersion);
    MimePart cached;
    assert(ensure_mime_tree(store, "opaque:key/123", "", cached));
    assert(cached.charset == "utf-8");
    cache->values["opaque:key/123:mime"] = "{\"v\":1,\"t\":\"old\"}";
    assert(ensure_mime_tree(store, "opaque:key/123", raw, cached));
    assert(cached.type == "text" && cached.metadata_version == kMimeMetadataVersion);
    FailingStore failing;
    assert(ensure_mime_tree(failing, "opaque", raw, tree));
    assert(!ensure_mime_tree(failing, "opaque", "", tree));
    assert(tree.type.empty());

    // An object key that happens to name a local file must not read or overwrite
    // that file's legacy sidecar when the actual provider has no cache capability.
    TempDirectory temp;
    const auto key = (temp.path / "object").string();
    MimePart local;
    parse_mime_tree("Content-Type: image/png\r\n\r\nimage", local);
    assert(save_mime_tree(key, local));
    auto provider = std::make_shared<pr::storage::NullStorageProvider>();
    auto no_cache = mime_metadata_store_for(provider);
    assert(!load_mime_tree(no_cache, key, tree));
    assert(ensure_mime_tree(no_cache, key, raw, tree));
    assert(tree.type == "text");
    assert(load_mime_tree(key, cached) && cached.type == "image");

    // A wrapper preserves the inner provider's explicit metadata capability.
    auto inner = std::make_shared<CacheProvider>();
    auto wrapped = std::make_shared<pr::storage::AsyncStorageProvider>(inner, nullptr);
    auto wrapped_cache = mime_metadata_store_for(wrapped);
    assert(ensure_mime_tree(wrapped_cache, "remote:mail/1", raw, tree));
    assert(load_mime_tree(wrapped_cache, "remote:mail/1", cached));
    assert(inner->values.count("remote:mail/1:mime") == 1);

    // Concurrent publication keeps every observed local sidecar complete.
    auto local_cache = std::make_shared<pr::storage::LocalFileMetadataCache>();
    CachedMimeMetadataStore local_store(local_cache);
    parse_mime_tree(raw, tree);
    assert(save_mime_tree(local_store, key, tree));
    std::thread writer([&] {
        for (int i = 0; i < 100; ++i) assert(save_mime_tree(local_store, key, tree));
    });
    for (int i = 0; i < 100; ++i) {
        MimePart observed;
        assert(load_mime_tree(local_store, key, observed));
        assert(observed.type == "text");
    }
    writer.join();
    std::string bytes;
    assert(!local_cache->load_metadata(key, "../escape", bytes));
    assert(!local_cache->save_metadata("", "mime", "data"));
    return 0;
}
