#pragma once
#include "framework/storage/i_object_metadata_cache.h"
#include <atomic>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <unistd.h>

namespace pr::storage {
// Explicit local adapter: only providers whose object keys are local file paths
// inherit this capability. Arbitrary object-store keys never reach this adapter.
class LocalFileMetadataCache : public IObjectMetadataCache {
public:
    bool load_metadata(const std::string& key, const std::string& name,
                       std::string& value) override {
        value.clear();
        if (!valid(key, name)) return false;
        try {
            std::ifstream file(key + "." + name, std::ios::binary);
            if (!file) return false;
            value.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            return !file.bad();
        } catch (...) { value.clear(); return false; }
    }
    bool save_metadata(const std::string& key, const std::string& name,
                       const std::string& value) override {
        if (!valid(key, name)) return false;
        static std::atomic<unsigned long long> sequence{0};
        const std::string path = key + "." + name;
        const std::string temporary = path + ".tmp." + std::to_string(::getpid()) + "." +
                                      std::to_string(sequence.fetch_add(1));
        try {
            {
                std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
                if (!file) return false;
                file.write(value.data(), static_cast<std::streamsize>(value.size()));
                file.flush();
                if (!file) { file.close(); std::remove(temporary.c_str()); return false; }
            }
            if (std::rename(temporary.c_str(), path.c_str()) == 0) return true;
        } catch (...) {}
        std::remove(temporary.c_str());
        return false;
    }
private:
    static bool valid(const std::string& key, const std::string& name) {
        return !key.empty() && key.find('\0') == std::string::npos &&
               !name.empty() && name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") ==
                                    std::string::npos;
    }
};
}
