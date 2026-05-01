#ifndef MAIL_SYSTEM_MAIL_CRYPTO_H
#define MAIL_SYSTEM_MAIL_CRYPTO_H

#include <openssl/evp.h>
#include <openssl/sha.h>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace mail_system {
namespace outbound {

// ---------- string helpers ----------

inline std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

inline std::string trim_ascii_ws(const std::string& input) {
    std::size_t begin = 0;
    while (begin < input.size() && (input[begin] == ' ' || input[begin] == '\t')) {
        ++begin;
    }
    std::size_t end = input.size();
    while (end > begin && (input[end - 1] == ' ' || input[end - 1] == '\t')) {
        --end;
    }
    return input.substr(begin, end - begin);
}

inline std::string collapse_ws(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    bool in_ws = false;
    for (unsigned char ch : input) {
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            if (!in_ws) {
                out.push_back(' ');
                in_ws = true;
            }
            continue;
        }
        in_ws = false;
        out.push_back(static_cast<char>(ch));
    }
    return trim_ascii_ws(out);
}

inline std::vector<std::string> split_lines_lf(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;
    current.reserve(text.size());
    for (char ch : text) {
        if (ch == '\n') {
            if (!current.empty() && current.back() == '\r') {
                current.pop_back();
            }
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
        if (current.back() == '\r') {
            current.pop_back();
        }
        lines.push_back(current);
    }
    return lines;
}

// ---------- DKIM canonicalization ----------

inline std::string canonicalize_header_relaxed(const std::string& field_name,
                                               const std::string& field_value) {
    return to_lower_copy(field_name) + ":" + collapse_ws(field_value) + "\r\n";
}

inline std::string normalize_body_simple(const std::string& body) {
    auto lines = split_lines_lf(body);
    while (!lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }
    if (lines.empty()) {
        return "\r\n";
    }
    std::ostringstream oss;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        oss << lines[i] << "\r\n";
    }
    return oss.str();
}

// ---------- crypto ----------

inline std::string base64_encode(const unsigned char* data, std::size_t size) {
    if (size == 0) {
        return {};
    }
    const int out_len = 4 * static_cast<int>((size + 2) / 3);
    std::string out(static_cast<std::size_t>(out_len), '\0');
    const int encoded = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(out.data()), data, static_cast<int>(size));
    if (encoded <= 0) {
        return {};
    }
    out.resize(static_cast<std::size_t>(encoded));
    return out;
}

inline std::string sha256_base64(const std::string& data) {
    unsigned char digest[SHA256_DIGEST_LENGTH] = {0};
    if (!EVP_Digest(data.data(), data.size(), digest, nullptr, EVP_sha256(), nullptr)) {
        return {};
    }
    return base64_encode(digest, SHA256_DIGEST_LENGTH);
}

} // namespace outbound
} // namespace mail_system

#endif // MAIL_SYSTEM_MAIL_CRYPTO_H
