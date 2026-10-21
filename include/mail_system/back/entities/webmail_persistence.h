#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mail_system::webmail {
struct MailActionResult { bool server_error = false; bool not_found = false; };
struct MailResourceScope {
    uint64_t user_id = 0;
    std::string address;
    uint64_t mailbox_id = 0;
    uint64_t mail_id = 0;
};
struct MailFlagUpdate {
    MailResourceScope scope;
    std::optional<bool> read;
    std::optional<bool> starred;
};
struct AuthorizedAttachment {
    bool server_error = false;
    bool not_found = false;
    int shard = -1;
    std::string object_key;
    std::string filename;
};
struct WebSessionInfo {
    uint64_t user_id = 0;
    std::string address;
    std::string credential_stamp;
    int shard = 0;
};
// Persistent lookup never returns or caches a password. credential_stamp is an
// opaque invalidation value, checked against the current account on every read.
struct WebSessionLookup {
    bool found = false;
    bool server_error = false;
    WebSessionInfo user;
};
inline bool valid_web_session_token(std::string_view token) noexcept {
    if (token.size() != 64) return false;
    for (const unsigned char c : token)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
} // namespace mail_system::webmail
