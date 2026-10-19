#ifndef PR_FRAMEWORK_LOG_FORMAT_H
#define PR_FRAMEWORK_LOG_FORMAT_H

#include <array>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace pr {
namespace detail {
template <typename T>
std::string log_argument(T&& value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::boolalpha;
    using Value = std::decay_t<T>;
    if constexpr (std::is_same_v<Value, unsigned char> || std::is_same_v<Value, signed char>)
        out << static_cast<int>(value);
    else if constexpr (std::is_same_v<Value, const char*> || std::is_same_v<Value, char*>)
        out << (value ? value : "(null)");
    else
        out << std::forward<T>(value);
    return out.str();
}
} // namespace detail

// Project logging uses sequential {} fields and escaped {{ / }} only.
// Deliberately not a fmt replacement: reject unsupported syntax instead of
// silently corrupting diagnostics. Formatting has no third-party dependency.
template <typename... Args>
std::string format_log(std::string_view format, Args&&... args) {
    std::array<std::string, sizeof...(Args)> values{
        detail::log_argument(std::forward<Args>(args))...};
    std::string result;
    size_t argument = 0;
    for (size_t i = 0; i < format.size(); ++i) {
        const char c = format[i];
        if ((c == '{' || c == '}') && i + 1 < format.size() && format[i + 1] == c) {
            result += c;
            ++i;
        } else if (c == '{' && i + 1 < format.size() && format[i + 1] == '}') {
            if (argument == values.size()) throw std::invalid_argument("log: missing argument");
            result += values[argument++];
            ++i;
        } else if (c == '{' || c == '}') {
            throw std::invalid_argument("log: only {} and escaped braces are supported");
        } else {
            result += c;
        }
    }
    if (argument != values.size()) throw std::invalid_argument("log: unused argument");
    return result;
}
} // namespace pr
#endif
