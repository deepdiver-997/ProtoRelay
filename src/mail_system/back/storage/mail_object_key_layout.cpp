#include "mail_system/back/storage/mail_object_key_layout.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <sstream>
#include <string>
#include <utility>

namespace mail_system::storage {
std::string normalize_mail_storage_root(std::string root) {
    if (!root.empty() && root.back() != '/' && root.back() != '\\') root.push_back('/');
    return root;
}
namespace {
// This is deliberately distinct from the SMTP display-filename sanitizer:
// persisted local/HDFS/distributed keys historically also replace ':' and '*'.
std::string sanitize_key_filename(const std::string& name) {
    if (name.empty()) return "attachment";
    std::string out;
    out.reserve(name.size());
    for (const char c : name) out.push_back(c == '/' || c == '\\' || c == ' ' || c == ':' || c == '*' ? '_' : c);
    return out;
}
IMailObjectKeyLayout& layout_for(pr::storage::IStorageProvider& provider) {
    auto* layout = dynamic_cast<IMailObjectKeyLayout*>(&provider);
    if (!layout) throw std::logic_error("storage provider has no mail object key layout");
    return *layout;
}
} // namespace
MailObjectKeyLayout::MailObjectKeyLayout(Kind kind, std::string mail_root, std::string attachment_root)
    : kind_(kind), mail_root_(normalize_mail_storage_root(std::move(mail_root))),
      attachment_root_(normalize_mail_storage_root(std::move(attachment_root))) {}
std::string MailObjectKeyLayout::build_mail_body_key(std::uint64_t id) {
    if (kind_ == Kind::discard) return "/dev/null/" + std::to_string(id);
    if (kind_ == Kind::s3) return "mail/" + std::to_string(id);
    // Local/HDFS/distributed historically used streams. Preserve their number
    // spelling too when an embedding process sets a non-default C++ locale.
    std::ostringstream key;
    key << (kind_ == Kind::local ? mail_root_ : "mail/") << id;
    return key.str();
}
std::string MailObjectKeyLayout::build_attachment_key(std::uint64_t id, const std::string& filename) {
    if (kind_ == Kind::discard) return "/dev/null/" + std::to_string(id) + "_" + filename;
    if (kind_ == Kind::s3) {
        const auto seq = attachment_seq_.fetch_add(1, std::memory_order_relaxed);
        return "attachments/" + std::to_string(id) + "/" + std::to_string(seq) + "_" + filename;
    }
    const auto seq = ++attachment_seq_;
    std::ostringstream key;
    if (kind_ == Kind::local) key << attachment_root_ << id << "_";
    else key << "attachment/" << id << "/";
    key << seq << "_" << sanitize_key_filename(filename);
    return key.str();
}
std::string build_mail_body_key(pr::storage::IStorageProvider& provider, std::uint64_t id) {
    return layout_for(provider).build_mail_body_key(id);
}
std::string build_attachment_key(pr::storage::IStorageProvider& provider, std::uint64_t id, const std::string& filename) {
    return layout_for(provider).build_attachment_key(id, filename);
}
std::string build_mail_body_key(const std::shared_ptr<pr::storage::IStorageProvider>& provider, std::uint64_t id) {
    if (!provider) throw std::logic_error("mail object key layout requires a storage provider");
    return build_mail_body_key(*provider, id);
}
std::string build_attachment_key(const std::shared_ptr<pr::storage::IStorageProvider>& provider,
                                 std::uint64_t id, const std::string& filename) {
    if (!provider) throw std::logic_error("mail object key layout requires a storage provider");
    return build_attachment_key(*provider, id, filename);
}
} // namespace mail_system::storage
