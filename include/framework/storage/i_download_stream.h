#pragma once
#include "framework/storage/io_error.h"
#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <fstream>

namespace pr {
// Bounded reads for HTTP downloads. One caller at a time, invoked on a worker.
class IDownloadStream {
public:
    virtual ~IDownloadStream() = default;
    virtual uint64_t size() const = 0;
    virtual bool read_at(uint64_t offset, char* buffer, size_t capacity, size_t& count, IoError& error) = 0;
};
class FileDownloadStream final : public IDownloadStream {
public:
    static std::unique_ptr<FileDownloadStream> open(const std::string& path, IoError& error) {
        auto stream = std::unique_ptr<FileDownloadStream>(new FileDownloadStream(path));
        if (!stream->file_) { error = IoError::permanent("download object unavailable"); return nullptr; }
        stream->file_.seekg(0, std::ios::end);
        auto length = stream->file_.tellg();
        if (length < 0) { error = IoError::retryable("download size unavailable"); return nullptr; }
        stream->size_ = static_cast<uint64_t>(length);
        return stream;
    }
    uint64_t size() const override { return size_; }
    bool read_at(uint64_t offset, char* buffer, size_t capacity, size_t& count, IoError& error) override {
        count = 0;
        if (offset > size_) { error = IoError::permanent("download offset exceeds size"); return false; }
        auto wanted = static_cast<size_t>(std::min<uint64_t>(capacity, size_ - offset));
        if (!wanted) return true;
        file_.clear(); file_.seekg(static_cast<std::streamoff>(offset));
        file_.read(buffer, static_cast<std::streamsize>(wanted));
        count = static_cast<size_t>(file_.gcount());
        if (count != wanted) { error = IoError::retryable("download object changed or read failed"); return false; }
        return true;
    }
private:
    explicit FileDownloadStream(const std::string& path) : file_(path, std::ios::binary) {}
    std::ifstream file_;
    uint64_t size_ = 0;
};
}
