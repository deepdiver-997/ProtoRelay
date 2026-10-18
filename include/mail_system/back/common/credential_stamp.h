#pragma once
#include <openssl/sha.h>
#include <string>
#include <string_view>
namespace mail_system {
inline std::string credential_stamp(std::string_view value) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(value.data()), value.size(), digest);
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(sizeof(digest) * 2);
    for (auto b : digest) {
        result.push_back(hex[b >> 4]);
        result.push_back(hex[b & 15]);
    }
    return result;
}
} // namespace mail_system
