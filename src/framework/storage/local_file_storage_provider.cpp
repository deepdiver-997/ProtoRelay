#include "framework/storage/local_file_storage_provider.h"

#include "framework/storage/local_file_read_stream.h"
#include "framework/storage/local_file_write_stream.h"

#include <string>
#include <memory>
#include <iterator>
#include <exception>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <utility>
#include <vector>

namespace pr {
namespace storage {

LocalFileStorageProvider::LocalFileStorageProvider(std::vector<std::string> directories)
    : directories_(std::move(directories)) {}

bool LocalFileStorageProvider::ensure_ready(IoError& error) {
    try {
        for (const auto& directory : directories_) {
            if (!directory.empty() && !std::filesystem::exists(directory))
                std::filesystem::create_directories(directory);
        }
        return true;
    } catch (const std::exception& e) {
        error = IoError::retryable(e.what());
        return false;
    }
}

bool LocalFileStorageProvider::append_binary(const std::string& storage_key,
                                             const char* data,
                                             std::size_t size,
                                             IoError& error) {
    if (storage_key.empty()) {
        error = IoError::permanent("storage key is empty");
        return false;
    }
    if (!data || size == 0) {
        return true;
    }

    try {
        const auto parent = std::filesystem::path(storage_key).parent_path();
        if (!parent.empty() && !std::filesystem::exists(parent)) {
            std::filesystem::create_directories(parent);
        }

        std::ofstream out(storage_key, std::ios::binary | std::ios::app);
        if (!out.is_open()) {
            error = IoError::retryable("failed to open file for append: " + storage_key);
            return false;
        }
        out.write(data, static_cast<std::streamsize>(size));
        if (!out.good()) {
            error = IoError::retryable("failed to append data: " + storage_key);
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        error = IoError::retryable(e.what());
        return false;
    }
}

std::unique_ptr<IWriteStream> LocalFileStorageProvider::open_write(const std::string& storage_key,
                                                                   IoError& error) {
    return LocalFileWriteStream::open(storage_key, error);
}

bool LocalFileStorageProvider::read_all(const std::string& storage_key,
                                        std::string& out,
                                        IoError& error) {
    if (storage_key.empty()) {
        error = IoError::permanent("storage key is empty");
        return false;
    }
    std::ifstream in(storage_key, std::ios::binary);
    if (!in.is_open()) {
        // 打不开以文件不存在居多：重试不会让它出现
        error = IoError::permanent("failed to open for read: " + storage_key);
        return false;
    }
    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return true;
}

std::unique_ptr<IReadStream> LocalFileStorageProvider::open_read(const std::string& storage_key,
                                                                  IoError& error) {
    return MappedReadStream::open(storage_key, error);
}

bool LocalFileStorageProvider::object_size(const std::string& storage_key,
                                           std::uint64_t& size,
                                           IoError& error) {
    if (storage_key.empty()) {
        error = IoError::permanent("storage key is empty");
        return false;
    }
    std::error_code ec;
    const auto sz = std::filesystem::file_size(storage_key, ec);
    if (ec) {
        // file_size 失败基本就是文件不存在（ENOENT）
        error = IoError::permanent("file_size failed for " + storage_key + ": " + ec.message());
        return false;
    }
    size = static_cast<std::uint64_t>(sz);
    return true;
}

bool LocalFileStorageProvider::remove_object(const std::string& storage_key,
                                             IoError& error) {
    if (storage_key.empty()) {
        return true;
    }

    if (std::remove(storage_key.c_str()) == 0) {
        return true;
    }

    if (std::filesystem::exists(storage_key)) {
        error = IoError::retryable("failed to remove object: " + storage_key);
        return false;
    }
    return true;
}

} // namespace storage
} // namespace pr
