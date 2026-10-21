#include "mail_system/back/webmail/mail_actions.h"
#include "framework/log/log_macros.h"
#include <exception>
#include <utility>

namespace mail_system::webmail {
namespace {
void run(const std::shared_ptr<pr::ThreadPoolBase>& workers, std::function<void()> task) {
    if (workers) workers->post(std::move(task)); else task();
}
}
void MailActions::update_async(uint64_t user, std::string address, uint64_t box, uint64_t mail,
        std::optional<bool> read, std::optional<bool> starred, std::function<void(MailActionResult)> cb) {
    auto repository = repository_;
    run(workers_, [repository, update = MailFlagUpdate{{user, std::move(address), box, mail}, read, starred},
                   cb = std::move(cb)] {
        MailActionResult out;
        try { out = repository->update_flags(update); }
        catch (const std::exception& error) {
            LOG_AT("WEBAPI", err, "flag repository exception: {}", error.what());
            out.server_error = true;
        }
        // Completion is outside the persistence catch: caller exceptions cannot
        // trigger rollback after commit or invoke the same callback twice.
        cb(out);
    });
}
void MailActions::download_async(uint64_t user, std::string address, uint64_t box, uint64_t mail, uint64_t attachment,
                               std::function<void(AttachmentDownload)> cb) {
    auto router = router_;
    auto repository = repository_;
    run(workers_, [router, repository, scope = MailResourceScope{user, std::move(address), box, mail}, attachment,
                   cb = std::move(cb)] {
        AttachmentDownload out;
        try {
            auto metadata = repository->find_attachment(scope, attachment);
            out.server_error = metadata.server_error;
            out.not_found = metadata.not_found;
            if (!out.server_error && !out.not_found) {
                auto provider = router && metadata.shard >= 0
                    ? router->get_storage(static_cast<size_t>(metadata.shard)) : nullptr;
                if (!provider) out.server_error = true;
                else {
                    pr::IoError error;
                    out.source = provider->open_download(metadata.object_key, error);
                    if (!out.source) {
                        LOG_AT("WEBAPI", warn, "download provider error attachment_id={} reason={}", attachment, error.message);
                        out.server_error = true;
                    } else out.filename = std::move(metadata.filename);
                }
            }
        } catch (const std::exception& error) {
            LOG_AT("WEBAPI", err, "download exception: {}", error.what());
            out.server_error = true;
        }
        cb(std::move(out));
    });
}
} // namespace mail_system::webmail
