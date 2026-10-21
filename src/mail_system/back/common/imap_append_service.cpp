#include "mail_system/back/common/imap_append_service.h"
#include "mail_system/back/algorithm/snow.h"
#include "mail_system/back/common/logger.h"
#include <cstdint>
#include <memory>
#include <string>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

namespace mail_system {
namespace {
struct AppendOperation : std::enable_shared_from_this<AppendOperation> {
    IImapAppendService::Lease lease;
    std::shared_ptr<pr::storage::IStorageProvider> storage;
    std::shared_ptr<pr::ThreadPoolBase> worker;
    std::shared_ptr<repository::IImapMessageRepository> messages;
    ImapAppendRequest request;
    IImapAppendService::Complete complete;
    uint64_t mail_id = 0;
    std::string body_key, address;

    bool remove_body() {
        if (body_key.empty()) return true;
        try {
            if (storage) {
                pr::IoError error;
                if (storage->remove_object(body_key, error)) return true;
                LOG_IMAP_ERROR("APPEND: failed to remove body {}: {}", body_key, error.message);
                return false;
            }
            std::error_code error;
            std::filesystem::remove(body_key, error);
            return !error;
        } catch (const std::exception& error) {
            LOG_IMAP_ERROR("APPEND: body compensation failed for {}: {}", body_key, error.what());
            return false;
        }
    }
    void fail(std::string error, bool cleaned) {
        ImapAppendResult result;
        result.error = std::move(error);
        result.cleanup_complete = cleaned;
        auto callback = std::move(complete);
        if (callback) callback(std::move(result));
    }
    void fail_after_body(std::string error, bool database_clean = true) {
        auto self = shared_from_this();
        worker->post([self, error = std::move(error), database_clean]() mutable {
            const bool body_clean = self->remove_body();
            self->fail(std::move(error), database_clean && body_clean);
        });
    }
    void write_body() {
        mail_id = static_cast<uint64_t>(algorithm::get_snowflake_generator().next_id());
        std::string failure;
        try {
            if (storage) {
                body_key = storage->build_mail_body_key(mail_id);
                pr::IoError error;
                if (!storage->append_binary(body_key, request.body.data(), request.body.size(), error))
                    failure = "Storage error: " + error.message;
            } else {
                body_key = "mail/" + std::to_string(mail_id);
                std::ofstream out(body_key, std::ios::binary);
                if (!out) failure = "Cannot write " + body_key;
                else {
                    out.write(request.body.data(), static_cast<std::streamsize>(request.body.size()));
                    if (!out) failure = "Write failed " + body_key;
                }
            }
        } catch (const std::exception& error) {
            failure = "Storage error: " + std::string(error.what());
        }
        if (!failure.empty()) { fail(std::move(failure), remove_body()); return; }
        repository::ImapMailRecord record;
        record.mail_id = mail_id;
        record.subject = request.subject;
        record.body_key = body_key;
        record.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        auto self = shared_from_this();
        messages->insert_mail_async(lease, std::move(record), [self](bool inserted) {
            if (!inserted) { self->fail_after_body("Insert mail record failed"); return; }
            self->lookup_address();
        });
    }
    void lookup_address() {
        auto self = shared_from_this();
        messages->get_user_address_async(lease, request.user_id, [self](std::string address) {
            self->address = address.empty() ? self->request.authenticated_address : std::move(address);
            repository::ImapMailLink link;
            link.mail_id = self->mail_id;
            link.user_id = self->request.user_id;
            link.mailbox_id = self->request.mailbox_id;
            link.sender = link.recipient = self->address;
            link.status = self->request.initial_status;
            self->messages->link_mail_async(self->lease, std::move(link), [self](bool linked) {
                if (linked) {
                    ImapAppendResult result;
                    result.ok = true;
                    result.mail_id = self->mail_id;
                    result.body_key = self->body_key;
                    result.address = self->address;
                    auto callback = std::move(self->complete);
                    if (callback) callback(std::move(result));
                    return;
                }
                self->messages->remove_mail_async(self->lease, self->mail_id, [self](bool cleaned) {
                    // An empty error preserves the existing generic tagged NO
                    // for linkage failures; cleanup status remains observable.
                    self->fail_after_body({}, cleaned);
                });
            });
        });
    }
};
class ImapAppendService final : public IImapAppendService {
public:
    ImapAppendService(std::shared_ptr<pr::ThreadPoolBase> worker,
                      std::shared_ptr<repository::IImapMessageRepository> messages)
        : worker_(std::move(worker)), messages_(std::move(messages)) {}
    void append_async(Lease lease, std::shared_ptr<pr::storage::IStorageProvider> storage,
                      ImapAppendRequest request, Complete complete) override {
        if (!lease || !lease->is_valid()) { if (complete) complete({false, 0, {}, {}, "DB connection failed", true}); return; }
        if (!worker_) { if (complete) complete({false, 0, {}, {}, "Server not ready", true}); return; }
        auto operation = std::make_shared<AppendOperation>();
        operation->lease = std::move(lease);
        operation->storage = std::move(storage);
        operation->worker = worker_;
        operation->messages = messages_;
        operation->request = std::move(request);
        operation->complete = std::move(complete);
        worker_->post([operation] { operation->write_body(); });
    }
private:
    std::shared_ptr<pr::ThreadPoolBase> worker_;
    std::shared_ptr<repository::IImapMessageRepository> messages_;
};
} // namespace
std::shared_ptr<IImapAppendService> make_imap_append_service(
    std::shared_ptr<pr::ThreadPoolBase> worker,
    std::shared_ptr<repository::IImapMessageRepository> messages) {
    if (!messages) messages = repository::make_sql_imap_message_repository();
    return std::make_shared<ImapAppendService>(std::move(worker), std::move(messages));
}
} // namespace mail_system
