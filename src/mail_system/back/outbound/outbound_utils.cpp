#include "mail_system/back/outbound/outbound_utils.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace mail_system {
namespace outbound {
namespace {

std::string sender_domain(const std::string& sender) {
    const auto at_pos = sender.find('@');
    if (at_pos == std::string::npos || at_pos + 1 >= sender.size()) {
        return "outbound.local";
    }
    return sender.substr(at_pos + 1);
}

std::string format_rfc5322_date_utc() {
    const auto now = std::time(nullptr);
    std::tm tm_utc{};
    gmtime_r(&now, &tm_utc);

    char buf[64] = {0};
    if (std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S +0000", &tm_utc) == 0) {
        return "Thu, 01 Jan 1970 00:00:00 +0000";
    }
    return std::string(buf);
}

std::string build_message_id(const OutboxRecord& record) {
    std::ostringstream oss;
    oss << "<" << record.mail_id << "." << record.id << "@" << sender_domain(record.sender) << ">";
    return oss.str();
}

std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string trim_ascii_ws(const std::string& input) {
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

std::string collapse_ws(const std::string& input) {
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

std::string canonicalize_header_relaxed(const std::string& field_name,
                                        const std::string& field_value) {
    return to_lower_copy(field_name) + ":" + collapse_ws(field_value) + "\r\n";
}

std::vector<std::string> split_lines_lf(const std::string& text) {
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
        if (!current.empty() && current.back() == '\r') {
            current.pop_back();
        }
        lines.push_back(current);
    }
    return lines;
}

std::string normalize_body_simple(const std::string& body) {
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

std::string base64_encode(const unsigned char* data, std::size_t size) {
    if (size == 0) {
        return {};
    }

    const int out_len = 4 * static_cast<int>((size + 2) / 3);
    std::string out(static_cast<std::size_t>(out_len), '\0');
    const int encoded = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data, static_cast<int>(size));
    if (encoded <= 0) {
        return {};
    }
    out.resize(static_cast<std::size_t>(encoded));
    return out;
}

std::string sha256_base64(const std::string& data) {
    unsigned char digest[SHA256_DIGEST_LENGTH] = {0};
    if (!EVP_Digest(data.data(), data.size(), digest, nullptr, EVP_sha256(), nullptr)) {
        return {};
    }
    return base64_encode(digest, SHA256_DIGEST_LENGTH);
}

bool sign_rsa_sha256_base64(const std::string& data,
                            const std::string& private_key_file,
                            std::string& signature_b64,
                            std::string& error_out) {
    FILE* key_fp = std::fopen(private_key_file.c_str(), "r");
    if (!key_fp) {
        error_out = "failed to open DKIM private key file: " + private_key_file;
        return false;
    }

    EVP_PKEY* pkey = PEM_read_PrivateKey(key_fp, nullptr, nullptr, nullptr);
    std::fclose(key_fp);
    if (!pkey) {
        error_out = "failed to parse DKIM private key (PEM)";
        return false;
    }

    EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) {
        EVP_PKEY_free(pkey);
        error_out = "failed to allocate EVP_MD_CTX";
        return false;
    }

    bool ok = false;
    do {
        if (EVP_DigestSignInit(md_ctx, nullptr, EVP_sha256(), nullptr, pkey) <= 0) {
            error_out = "EVP_DigestSignInit failed";
            break;
        }
        if (EVP_DigestSignUpdate(md_ctx, data.data(), data.size()) <= 0) {
            error_out = "EVP_DigestSignUpdate failed";
            break;
        }

        std::size_t sig_len = 0;
        if (EVP_DigestSignFinal(md_ctx, nullptr, &sig_len) <= 0 || sig_len == 0) {
            error_out = "EVP_DigestSignFinal size query failed";
            break;
        }

        std::vector<unsigned char> signature(sig_len);
        if (EVP_DigestSignFinal(md_ctx, signature.data(), &sig_len) <= 0) {
            error_out = "EVP_DigestSignFinal failed";
            break;
        }
        signature.resize(sig_len);
        signature_b64 = base64_encode(signature.data(), signature.size());
        if (signature_b64.empty()) {
            error_out = "failed to base64-encode DKIM signature";
            break;
        }
        ok = true;
    } while (false);

    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);
    return ok;
}

std::string join_header_names_colon(const std::vector<std::string>& names) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < names.size(); ++i) {
        oss << names[i];
        if (i + 1 < names.size()) {
            oss << ":";
        }
    }
    return oss.str();
}

std::string build_dkim_header(const std::unordered_map<std::string, std::string>& headers,
                              const std::string& canonical_body,
                              const OutboundIdentityConfig& identity_config,
                              std::string& error_out) {
    const std::string selector = trim_ascii_ws(identity_config.dkim_selector);
    const std::string domain = trim_ascii_ws(identity_config.dkim_domain);
    const std::string key_file = trim_ascii_ws(identity_config.dkim_private_key_file);
    if (selector.empty() || domain.empty() || key_file.empty()) {
        error_out = "dkim config missing selector/domain/private_key_file";
        return {};
    }

    static const std::vector<std::string> signed_headers = {
        "from",
        "to",
        "subject",
        "date",
        "message-id",
        "mime-version",
        "content-type",
        "content-transfer-encoding",
        "reply-to",
    };

    const std::string body_hash = sha256_base64(canonical_body);
    if (body_hash.empty()) {
        error_out = "failed to compute DKIM body hash";
        return {};
    }

    const std::string signed_header_list = join_header_names_colon(signed_headers);
    const std::string timestamp = std::to_string(static_cast<long long>(std::time(nullptr)));

    std::ostringstream dkim_value_without_b;
    dkim_value_without_b << "v=1; a=rsa-sha256; c=relaxed/simple; d=" << domain
                         << "; s=" << selector
                         << "; t=" << timestamp
                         << "; h=" << signed_header_list
                         << "; bh=" << body_hash
                         << "; b=";

    std::string signing_input;
    for (const auto& name : signed_headers) {
        auto it = headers.find(name);
        if (it == headers.end()) {
            error_out = "missing header for DKIM signing: " + name;
            return {};
        }
        signing_input += canonicalize_header_relaxed(name, it->second);
    }
    signing_input += canonicalize_header_relaxed("DKIM-Signature", dkim_value_without_b.str());

    std::string signature_b64;
    if (!sign_rsa_sha256_base64(signing_input, key_file, signature_b64, error_out)) {
        return {};
    }

    return "DKIM-Signature: " + dkim_value_without_b.str() + signature_b64 + "\r\n";
}

} // namespace

std::string build_outbound_message(const OutboxRecord& record,
                                   const std::string& header_from,
                                   const OutboundIdentityConfig& identity_config,
                                   bool* dkim_applied,
                                   std::string* dkim_error) {
    if (dkim_applied) {
        *dkim_applied = false;
    }
    if (dkim_error) {
        dkim_error->clear();
    }

    const std::string subject = "Outbound relay test";
    const std::string to_value = "<" + record.recipient + ">";
    const std::string from_value = "<" + header_from + ">";
    const std::string reply_to_value = "<" + record.sender + ">";
    const std::string date_value = format_rfc5322_date_utc();
    const std::string message_id = build_message_id(record);
    const std::string mime_version = "1.0";
    const std::string content_type = "text/plain; charset=UTF-8";
    const std::string content_transfer_encoding = "8bit";
    const std::string body = "relayed by outbound client, outbox_id=" + std::to_string(record.id) + "\r\n";
    const std::string canonical_body = normalize_body_simple(body);

    std::unordered_map<std::string, std::string> dkim_headers;
    dkim_headers["from"] = from_value;
    dkim_headers["to"] = to_value;
    dkim_headers["subject"] = subject;
    dkim_headers["date"] = date_value;
    dkim_headers["message-id"] = message_id;
    dkim_headers["mime-version"] = mime_version;
    dkim_headers["content-type"] = content_type;
    dkim_headers["content-transfer-encoding"] = content_transfer_encoding;
    dkim_headers["reply-to"] = reply_to_value;

    std::ostringstream wire;
    if (identity_config.dkim_enabled) {
        std::string local_dkim_error;
        const std::string dkim_header = build_dkim_header(dkim_headers, canonical_body, identity_config, local_dkim_error);
        if (!dkim_header.empty()) {
            if (dkim_applied) {
                *dkim_applied = true;
            }
            wire << dkim_header;
        } else if (dkim_error) {
            *dkim_error = std::move(local_dkim_error);
        }
    }

    wire << "Subject: " << subject << "\r\n"
         << "From: " << from_value << "\r\n"
         << "To: " << to_value << "\r\n"
         << "Reply-To: " << reply_to_value << "\r\n"
         << "Date: " << date_value << "\r\n"
         << "Message-ID: " << message_id << "\r\n"
         << "MIME-Version: " << mime_version << "\r\n"
         << "Content-Type: " << content_type << "\r\n"
         << "Content-Transfer-Encoding: " << content_transfer_encoding << "\r\n"
         << "\r\n"
         << canonical_body
         << ".\r\n";

    return wire.str();
}

} // namespace outbound
} // namespace mail_system
