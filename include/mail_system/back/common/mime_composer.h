#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mail_system::mime {

inline constexpr size_t kMaxBodyBytes = 512 * 1024;
inline constexpr size_t kMaxRecipients = 100;

// MVP：ASCII addr-spec + UTF-8 主题/正文。附件上传后置。
struct Message {
    std::string from; // 调用方只能从已认证会话填入
    std::vector<std::string> to, cc;
    std::string subject;
    std::optional<std::string> text, html;
};

struct ComposedMessage {
    int error_status = 0;
    std::string error;
    std::string raw;
    std::string message_id;
    std::vector<std::string> recipients; // 去重后的 SMTP envelope
};

bool valid_address(std::string_view address);
ComposedMessage compose(const Message& message);

} // namespace mail_system::mime
