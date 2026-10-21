#pragma once
#include <cstdint>
#include <ctime>
#include <string>

namespace mail_system {
struct MailboxSummary {
    uint64_t id = 0;
    std::string name;
    int box_type = 0;
};
// Application data shared by protocol adapters and repository implementations.
struct MailboxMailInfo {
    uint64_t mail_id = 0;
    std::string sender, recipient, subject, body_path;
    bool is_starred = false, is_deleted = false, is_important = false;
    int status = 0;
    time_t send_time = 0;
};
} // namespace mail_system
