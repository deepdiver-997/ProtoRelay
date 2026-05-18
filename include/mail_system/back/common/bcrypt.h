#ifndef MAIL_SYSTEM_BCRYPT_H
#define MAIL_SYSTEM_BCRYPT_H

#include <string>

namespace mail_system {

// Hash a password with a random salt using bcrypt.
// cost: work factor (4-31, default 12). Each increment doubles the work.
// Returns a string in the format $2b$[cost]$[22-char-salt][31-char-hash]
// Returns empty string on error (e.g. OpenSSL RAND_bytes failure).
std::string bcrypt_hash(const std::string& password, unsigned int cost = 12);

// Verify a password against a bcrypt hash.
// hash must be in the format $2b$[cost]$[22-char-salt][31-char-hash]
bool bcrypt_verify(const std::string& password, const std::string& hash);

} // namespace mail_system

#endif // MAIL_SYSTEM_BCRYPT_H
