#pragma once
#include "framework/storage/io_error.h"
#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>

namespace pr {
// Bounded reads for HTTP downloads. One caller at a time, invoked on a worker.
class IDownloadStream {
public:
    virtual ~IDownloadStream() = default;
    virtual uint64_t size() const = 0;
    virtual bool read_at(uint64_t offset, char* buffer, size_t capacity, size_t& count, IoError& error) = 0;
};
}
