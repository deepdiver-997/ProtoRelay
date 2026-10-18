#pragma once

#include <string>
#include <string_view>

namespace mail_system::smtp {

// SMTP DATA transparency，供出站投递和 webmail submission 共用。
inline std::string dot_stuff(std::string_view body) {
    std::string out;
    out.reserve(body.size() + body.size() / 50);
    for (size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '.' && (i == 0 || body[i - 1] == '\n')) out.push_back('.');
        out.push_back(body[i]);
    }
    return out;
}

inline std::string data_frame(std::string_view body) {
    auto out = dot_stuff(body);
    if (out.size() < 2 || out.compare(out.size() - 2, 2, "\r\n") != 0)
        out += "\r\n";
    out += ".\r\n";
    return out;
}

} // namespace mail_system::smtp
