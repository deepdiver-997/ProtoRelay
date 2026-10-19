#ifndef PR_FRAMEWORK_NET_IO_TYPES_H
#define PR_FRAMEWORK_NET_IO_TYPES_H
#include <cstddef>
#include <string>
#include <system_error>
#include <type_traits>

namespace pr::net {
class MutableBuffer {
public:
    MutableBuffer() = default;
    MutableBuffer(void* data, size_t size) : data_(data), size_(size) {}
    void* data() const { return data_; }
    size_t size() const { return size_; }
private:
    void* data_ = nullptr;
    size_t size_ = 0;
};
class ConstBuffer {
public:
    ConstBuffer() = default;
    ConstBuffer(const void* data, size_t size) : data_(data), size_(size) {}
    ConstBuffer(MutableBuffer buffer) : data_(buffer.data()), size_(buffer.size()) {}
    const void* data() const { return data_; }
    size_t size() const { return size_; }
private:
    const void* data_ = nullptr;
    size_t size_ = 0;
};
// Views do not own storage. Callers keep it alive until operation completion.
template <typename Container>
MutableBuffer buffer(Container& value) { return {value.data(), value.size() * sizeof(typename Container::value_type)}; }
template <typename Container>
ConstBuffer buffer(const Container& value) { return {value.data(), value.size() * sizeof(typename Container::value_type)}; }
inline MutableBuffer buffer(void* data, size_t size) { return {data, size}; }
inline ConstBuffer buffer(const void* data, size_t size) { return {data, size}; }

enum class HandshakeRole { client, server };
enum class IoErrc { end_of_stream = 1 };

class IoErrorCategory final : public std::error_category {
public:
    const char* name() const noexcept override { return "protorelay.io"; }
    std::string message(int value) const override {
        return value == static_cast<int>(IoErrc::end_of_stream) ? "End of stream" : "Unknown IO error";
    }
};
inline std::error_code make_error_code(IoErrc value) {
    static const IoErrorCategory category;
    return {static_cast<int>(value), category};
}
} // namespace pr::net
namespace std { template <> struct is_error_code_enum<pr::net::IoErrc> : true_type {}; }
#endif
