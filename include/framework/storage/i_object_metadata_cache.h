#pragma once
#include <string>

namespace pr::storage {
// Optional, fast metadata cache capability. Keys are interpreted by the provider.
// A cache miss or failed write never changes the object's availability. Implementations
// publish complete values atomically and must be safe for concurrent readers/writers.
// Remote providers may omit this capability rather than block an IO thread on cache IO.
class IObjectMetadataCache {
public:
    virtual ~IObjectMetadataCache() = default;
    virtual bool load_metadata(const std::string& object_key, const std::string& name,
                               std::string& value) = 0;
    virtual bool save_metadata(const std::string& object_key, const std::string& name,
                               const std::string& value) = 0;
};
}
