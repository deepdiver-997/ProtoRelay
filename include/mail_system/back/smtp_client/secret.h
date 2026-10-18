#pragma once
#include <memory>
#include <openssl/crypto.h>
#include <string_view>
#include <vector>

namespace mail_system::smtp {
// 共享凭据只存在内存，最后一个引用释放时擦除；内核不依赖 Webmail 会话。
class Secret {
public:
    explicit Secret(std::string_view value) : bytes_(value.begin(), value.end()) {}
    ~Secret() { if (!bytes_.empty()) OPENSSL_cleanse(bytes_.data(), bytes_.size()); }
    Secret(const Secret&) = delete;
    Secret& operator=(const Secret&) = delete;
    std::string_view view() const { return {bytes_.data(), bytes_.size()}; }
private:
    std::vector<char> bytes_;
};

using SecretPtr = std::shared_ptr<const Secret>;
} // namespace mail_system::smtp
