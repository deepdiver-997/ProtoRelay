#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <string>

namespace mail_system {
namespace mime_params_detail {
inline std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
inline std::string trim(const std::string& value) {
    auto first = value.find_first_not_of(" \t"), last = value.find_last_not_of(" \t");
    return first == std::string::npos ? std::string() : value.substr(first, last - first + 1);
}
inline std::string base64(const std::string& value) {
    constexpr char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < value.size(); i += 3) {
        uint32_t bits = static_cast<unsigned char>(value[i]) << 16;
        if (i + 1 < value.size()) bits |= static_cast<unsigned char>(value[i + 1]) << 8;
        if (i + 2 < value.size()) bits |= static_cast<unsigned char>(value[i + 2]);
        out += chars[(bits >> 18) & 63];out += chars[(bits >> 12) & 63];
        out += i + 1 < value.size() ? chars[(bits >> 6) & 63] : '=';
        out += i + 2 < value.size() ? chars[bits & 63] : '=';
    }
    return out;
}
} // namespace mime_params_detail

inline std::string mime_header_value(const std::string& headers, const std::string& name) {
    std::string result;
    bool selected = false;
    for (size_t pos = 0; pos < headers.size();) {
        auto end = headers.find('\n', pos);
        if (end == std::string::npos) end = headers.size();
        auto line = headers.substr(pos, end - pos);
        pos = end == headers.size() ? end : end + 1;
        if (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            if (selected) result += " " + mime_params_detail::trim(line.back() == '\r' ? line.substr(0, line.size() - 1) : line);
            continue;
        }
        if (selected) break;
        auto colon = line.find(':');
        if (colon != std::string::npos && mime_params_detail::lower(line.substr(0, colon)) == name) {
            selected = true;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            result = mime_params_detail::trim(line.substr(colon + 1));
        }
    }
    return result;
}

inline std::string mime_percent_decode(const std::string& value) {
    auto hex = [](unsigned char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string result;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            int h = hex(value[i + 1]), l = hex(value[i + 2]);
            if (h >= 0 && l >= 0 && h * 16 + l != 0) {
                result += static_cast<char>(h * 16 + l);i += 2;continue;
            }
        }
        result += value[i];
    }
    return result;
}

// MIME 参数保留原值大小写；支持 quoted-pair、RFC 2231 扩展及连续分段。
inline std::string mime_parameter(const std::string& header, const std::string& name) {
    std::map<std::string, std::string> params;
    auto pos = header.find(';');
    while (pos != std::string::npos && pos < header.size()) {
        ++pos;
        while (pos < header.size() && (header[pos] == ' ' || header[pos] == '\t')) ++pos;
        auto eq = header.find('=', pos), semi = header.find(';', pos);
        if (eq == std::string::npos || (semi != std::string::npos && semi < eq)) { pos = semi;continue; }
        auto key = mime_params_detail::lower(mime_params_detail::trim(header.substr(pos, eq - pos)));
        pos = eq + 1;
        while (pos < header.size() && (header[pos] == ' ' || header[pos] == '\t')) ++pos;
        std::string value;
        if (pos < header.size() && header[pos] == '"') {
            ++pos;
            while (pos < header.size() && header[pos] != '"') {
                if (header[pos] == '\\' && pos + 1 < header.size()) ++pos;
                value += header[pos++];
            }
            if (pos < header.size()) ++pos;
            pos = header.find(';', pos);
        } else {
            auto end = header.find(';', pos);
            value = mime_params_detail::trim(header.substr(pos, end == std::string::npos ? end : end - pos));
            pos = end;
        }
        params.emplace(std::move(key), std::move(value));
    }
    std::string value, charset;
    bool extended = false;
    auto first = params.find(name + "*0*");
    if (first == params.end()) first = params.find(name + "*0");
    if (first != params.end()) {
        for (size_t index = 0; index <= params.size(); ++index) {
            auto part = params.find(name + "*" + std::to_string(index) + "*");
            bool encoded = part != params.end();
            if (!encoded) part = params.find(name + "*" + std::to_string(index));
            if (part == params.end()) break;
            auto piece = part->second;
            if (index == 0 && encoded) {
                auto a = piece.find('\''), b = a == std::string::npos ? a : piece.find('\'', a + 1);
                if (b != std::string::npos) { charset = piece.substr(0, a);piece.erase(0, b + 1); }
            }
            value += encoded ? mime_percent_decode(piece) : piece;
        }
        extended = true;
    } else if (auto item = params.find(name + "*"); item != params.end()) {
        auto piece = item->second;
        auto a = piece.find('\''), b = a == std::string::npos ? a : piece.find('\'', a + 1);
        if (b != std::string::npos) { charset = piece.substr(0, a);piece.erase(0, b + 1); }
        value = mime_percent_decode(piece);extended = true;
    }
    if (!extended) {
        auto item = params.find(name);return item == params.end() ? std::string() : item->second;
    }
    auto lower = mime_params_detail::lower(charset);
    if (!charset.empty() && lower != "utf-8" && lower != "us-ascii") {
        // 未转换的字符集以 encoded-word 交给客户端，避免把 GBK 等字节误称 UTF-8。
        return "=?" + charset + "?B?" + mime_params_detail::base64(value) + "?=";
    }
    return value;
}

} // namespace mail_system
