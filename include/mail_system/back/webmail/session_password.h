#pragma once

#include <memory>
#include <openssl/crypto.h>
#include <string_view>
#include <vector>

namespace mail_system::webmail {

// 587 必须用用户凭据 AUTH。只在进程内随会话持有，绝不序列化/落盘/记日志。
// 禁止拷贝密码；shared_ptr 让已发起的提交保活，最后引用释放时擦除缓冲。
class SessionPassword {
public:
    explicit SessionPassword(std::string_view value) : bytes_(value.begin(), value.end()) {}
    ~SessionPassword() { if (!bytes_.empty()) OPENSSL_cleanse(bytes_.data(), bytes_.size()); }
    SessionPassword(const SessionPassword&) = delete;
    SessionPassword& operator=(const SessionPassword&) = delete;
    std::string_view view() const { return {bytes_.data(), bytes_.size()}; }
private:
    std::vector<char> bytes_;
};

using SessionPasswordPtr = std::shared_ptr<const SessionPassword>;

} // namespace mail_system::webmail
