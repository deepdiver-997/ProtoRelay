#include "mail_system/back/common/mime_composer.h"
#include "mail_system/back/common/mail_crypto.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <openssl/rand.h>
#include <unordered_set>

namespace mail_system::mime {
namespace {

bool alnum(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

std::string random_hex() {
    unsigned char bytes[24];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) return {};
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (auto b : bytes) { out += hex[b >> 4]; out += hex[b & 15]; }
    return out;
}

std::string base64(std::string_view value) {
    return outbound::base64_encode(reinterpret_cast<const unsigned char*>(value.data()), value.size());
}

std::string encoded_subject(const std::string& value) {
    if (value.empty()) return "Subject:\r\n";
    std::string out = "Subject: ";
    for (size_t pos = 0; pos < value.size();) {
        size_t end = std::min(pos + 39, value.size());
        // encoded-word 不截断 UTF-8 字符；每段含语法不超过 75 字节。
        while (end < value.size() && (static_cast<unsigned char>(value[end]) & 0xc0) == 0x80) --end;
        if (end == pos) return {}; // 无效 UTF-8（HTTP JSON parser 正常不会交付此输入）
        if (pos) out += "\r\n ";
        out += "=?UTF-8?B?" + base64(std::string_view(value).substr(pos, end - pos)) + "?=";
        pos = end;
    }
    return out + "\r\n";
}

std::string address_header(const char* name, const std::vector<std::string>& addresses) {
    if (addresses.empty()) return {};
    std::string out = std::string(name) + ": ";
    for (size_t i = 0; i < addresses.size(); ++i) {
        if (i) out += ",\r\n ";
        out += "<" + addresses[i] + ">";
    }
    return out + "\r\n";
}

std::string text_part(const std::string& text, const char* subtype) {
    // 规范化行尾后编码，避免裸换行/超长行/DATA 终止符进入 wire。
    std::string canonical;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' || text[i] == '\n') {
            if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            canonical += "\r\n";
        } else canonical += text[i];
    }
    std::string out = std::string("Content-Type: text/") + subtype +
        "; charset=utf-8\r\nContent-Transfer-Encoding: base64\r\n\r\n";
    const auto encoded = base64(canonical);
    for (size_t pos = 0; pos < encoded.size(); pos += 76)
        out += encoded.substr(pos, 76) + "\r\n";
    return out;
}

std::string date_header() {
    static constexpr const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static constexpr const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const auto now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    char buf[80];
    std::snprintf(buf, sizeof(buf), "Date: %s, %02d %s %04d %02d:%02d:%02d +0000\r\n",
        days[utc.tm_wday], utc.tm_mday, months[utc.tm_mon], utc.tm_year + 1900,
        utc.tm_hour, utc.tm_min, utc.tm_sec);
    return buf;
}

} // namespace

bool valid_address(std::string_view address) {
    if (address.size() > 254) return false;
    const auto at = address.find('@');
    if (at == std::string_view::npos || at == 0 || at > 64) return false;
    const auto local = address.substr(0, at), domain = address.substr(at + 1);
    if (local.front() == '.' || local.back() == '.' || local.find("..") != std::string_view::npos ||
        domain.empty() || domain.size() > 253) return false;
    for (unsigned char c : local)
        if (!alnum(c) && std::string_view(".!#$%&'*+-/=?^_`{|}~").find(c) == std::string_view::npos)
            return false;
    size_t label = 0;
    for (size_t i = 0; i <= domain.size(); ++i) {
        if (i == domain.size() || domain[i] == '.') {
            if (i == label || i - label > 63 || domain[label] == '-' || domain[i - 1] == '-') return false;
            label = i + 1;
        } else if (!alnum(static_cast<unsigned char>(domain[i])) && domain[i] != '-') return false;
    }
    return true;
}

ComposedMessage compose(const Message& m) {
    ComposedMessage out;
    auto fail = [&out](int status, const char* error) {
        out.error_status = status;
        out.error = error;
        return out;
    };
    if (!valid_address(m.from)) return fail(400, "invalid sender address");
    const auto count = m.to.size() + m.cc.size();
    if (!count || count > kMaxRecipients) return fail(400, "expected 1..100 recipients");
    std::unordered_set<std::string> seen;
    for (const auto* list : {&m.to, &m.cc}) {
        for (const auto& address : *list) {
            if (!valid_address(address)) return fail(400, "invalid recipient address");
            // users/mail_recipients 的地址唯一键不区分大小写；同账号只发一次 RCPT。
            std::string key = address;
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
                return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
            });
            if (seen.insert(key).second) out.recipients.push_back(address);
        }
    }
    if (m.subject.size() > 512) return fail(400, "subject exceeds 512 bytes");
    for (unsigned char c : m.subject)
        if (c < 32 || c == 127) return fail(400, "invalid subject");
    if (!m.text && !m.html) return fail(400, "text or html required");
    if ((m.text ? m.text->size() : 0) + (m.html ? m.html->size() : 0) > kMaxBodyBytes)
        return fail(413, "message body exceeds 512 KiB");
    const auto subject = encoded_subject(m.subject);
    if (subject.empty()) return fail(400, "invalid UTF-8 subject");
    const auto nonce = random_hex();
    if (nonce.empty()) return fail(500, "message id generation failed");
    out.message_id = "<" + nonce + "@" + m.from.substr(m.from.find('@') + 1) + ">";
    out.raw = date_header() + "Message-ID: " + out.message_id + "\r\nFrom: <" + m.from + ">\r\n";
    out.raw += address_header("To", m.to) + address_header("Cc", m.cc);
    out.raw += subject + "MIME-Version: 1.0\r\n";
    if (m.text && m.html) {
        const std::string boundary = "wb_" + nonce;
        out.raw += "Content-Type: multipart/alternative; boundary=\"" + boundary + "\"\r\n\r\n";
        out.raw += "--" + boundary + "\r\n" + text_part(*m.text, "plain") + "\r\n";
        out.raw += "--" + boundary + "\r\n" + text_part(*m.html, "html") + "\r\n";
        out.raw += "--" + boundary + "--\r\n";
    } else {
        out.raw += m.text ? text_part(*m.text, "plain") : text_part(*m.html, "html");
    }
    return out;
}

} // namespace mail_system::mime
