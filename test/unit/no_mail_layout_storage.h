#pragma once
#include "framework/storage/null_storage_provider.h"
#include <cstddef>
#include <memory>
#include <string>

namespace mail_system::test {
// Generic IO provider with deliberately no application naming capability.
class NoMailLayoutStorage final : public pr::storage::NullStorageProvider {
public:
    size_t io_calls = 0;
    bool append_binary(const std::string&, const char*, size_t, pr::IoError&) override { ++io_calls; return true; }
    bool read_all(const std::string&, std::string&, pr::IoError&) override { ++io_calls; return false; }
    bool remove_object(const std::string&, pr::IoError&) override { ++io_calls; return true; }
    std::unique_ptr<pr::IWriteStream> open_write(const std::string&, pr::IoError&) override { ++io_calls; return {}; }
};
} // namespace mail_system::test
