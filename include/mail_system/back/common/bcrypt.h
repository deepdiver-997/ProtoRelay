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

// =========================================================================
// 存储格式自动识别 —— SMTP / IMAP / POP3 三条认证链共用
//
// users.password 列历史上混有三种格式：
//   bcrypt  "$2a$/$2b$/$2y$…"（60 字符）—— register-service / hash_tool 产出
//   MD5     32 位小写十六进制 —— 早期导入数据
//   明文    其它任意字符串 —— 手工 INSERT / 临时重置
// 统一之前三个协议各自手写判断且口径不一：SMTP 认 MD5，IMAP/POP3 只认 bcrypt
// 与明文 → 同一个 MD5 账号能发信却登不上收信端。
// =========================================================================

// stored 是否为 bcrypt（只看 "$2" 前缀，与 bcrypt_verify 的识别口径一致）
bool is_bcrypt_hash(const std::string& stored);

// 按 stored 的格式自动选择校验方式：bcrypt → bcrypt_verify；32 位小写 hex →
// MD5 比对；其余 → 明文等值比对。
// 口径副作用：明文密码本身恰为 32 位小写 hex 的账号会被当 MD5 校验而登不上，
// 这是既有 SMTP 行为，统一后三协议一致。
bool verify_password_hash(const std::string& input, const std::string& stored);

// rehash-on-login 触发条件：stored 不是 bcrypt（MD5 / 明文历史行）。
// 调用方在 verify_password_hash 为 true 之后用 bcrypt_hash(input) 回写 DB 并刷新
// 缓存条目，存储格式随用户登录逐账号收敛到 bcrypt（见 common/password_rehash.h）。
bool password_needs_rehash(const std::string& stored);

} // namespace mail_system

#endif // MAIL_SYSTEM_BCRYPT_H
