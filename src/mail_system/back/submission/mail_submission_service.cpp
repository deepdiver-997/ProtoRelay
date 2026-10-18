#include "mail_system/back/submission/mail_submission_service.h"
#include "mail_system/back/storage/mail_body_writer.h"
#include "mail_system/back/common/mime_parser.h"
#include "mail_system/back/algorithm/smtp_utils.h"
#include <unordered_set>

namespace mail_system::submission {
struct MailSubmissionService::Operation : std::enable_shared_from_this<Operation> {
    std::shared_ptr<MailSubmissionService> service;
    Request request;
    Complete complete;
    std::vector<std::string> recipients;
    std::unordered_set<std::string> seen;
    size_t index = 0;
    int shard = 0;
    std::unique_ptr<mail> data;
    std::shared_ptr<storage::IStorageProvider> storage;
    std::shared_ptr<storage::MailBodyWriter> writer;
    bool body_committed = false;
    bool admission_started = false;
    void guard(std::function<void()> task) {
        try {
            task();
        } catch (const std::exception &e) {
            LOG_AT("SUBMISSION", err, "submission exception user_id={} reason={}", request.user_id, e.what());
            fail(admission_started ? Status::Unknown : Status::Unavailable, 451, "internal");
        }
    }

    void fail(Status status, int code, std::string stage) {
        LOG_AT("SUBMISSION", warn, "submission failed user_id={} stage={} code={}", request.user_id, stage,
               code);
        if (body_committed && data && storage) {
            storage::IoError error;
            if (!storage->remove_object(data->body_path, error))
                LOG_AT("SUBMISSION", err, "unadmitted body cleanup failed mail_id={} reason={}", data->id,
                       error.message);
            storage->remove_object(data->body_path + ".mime", error);
            body_committed = false;
        }
        auto cb = std::move(complete);
        if (cb)
            cb({status, code, std::move(stage)});
    }
    void expand_next() {
        if (index == request.recipients.size()) {
            write_body();
            return;
        }
        auto address = request.recipients[index++];
        const auto &cfg = service->config_;
        if (!AccountPolicy::recipient_allowed(address, cfg.system_domain, cfg.external_delivery_enabled)) {
            fail(Status::Rejected, 550, "rcpt");
            return;
        }
        auto add = [self = shared_from_this(), address](std::vector<std::string> targets) {
            if (targets.empty())
                targets.push_back(address); // preserve SMTP submission alias fallback
            for (auto &target : targets) {
                auto key = algorithm::to_lower(target);
                if (self->seen.insert(key).second)
                    self->recipients.push_back(std::move(target));
            }
            self->service->workers_->post([self] { self->guard([&] { self->expand_next(); }); });
        };
        auto at = address.rfind('@');
        bool local = at != std::string::npos &&
                     algorithm::to_lower(address.substr(at + 1)) == algorithm::to_lower(cfg.system_domain);
        if (local)
            service->policy_->alias_targets_async(address, std::move(add));
        else
            add({});
    }
    void write_body() {
        // Provider open/write/commit and MIME parsing must not occupy a DB or IO callback thread.
        auto self = shared_from_this();
        service->workers_->post([self] {
            self->guard([&] {
                auto &s = *self->service;
                if (s.queue_->is_shutdown()) {
                    self->fail(Status::Unavailable, 451, "queue");
                    return;
                }
                self->data = std::make_unique<mail>();
                auto &m = *self->data;
                m.id = s.ids_.next_id();
                m.from = self->request.sender;
                m.to = self->recipients;
                for (size_t i = 0; i < m.to.size(); ++i)
                    m.ids.push_back(s.ids_.next_id());
                m.subject = self->request.subject;
                m.source_message_id = self->request.message_id;
                m.send_time = std::time(nullptr);
                m.box_id = 1;
                m.status = 1;
                m.mail_over = true;
                self->storage = s.router_->get_storage(static_cast<size_t>(self->shard));
                if (!self->storage) {
                    self->fail(Status::Unavailable, 451, "storage");
                    return;
                }
                m.body_path = self->storage->build_mail_body_key(m.id);
                storage::IoError error;
                auto stream = self->storage->open_write(m.body_path, error);
                if (!stream) {
                    LOG_AT("SUBMISSION", err, "open body failed mail_id={} reason={}", m.id, error.message);
                    self->fail(Status::Unavailable, 451, "storage");
                    return;
                }
                self->writer = std::make_shared<storage::MailBodyWriter>(std::move(stream));
                if (!self->writer->write(self->request.raw.data(), self->request.raw.size(), error)) {
                    LOG_AT("SUBMISSION", err, "write body failed mail_id={} reason={}", m.id, error.message);
                    self->fail(Status::Unavailable, 451, "storage");
                    return;
                }
                self->writer->commit_async([self, writer = self->writer](bool ok, const storage::IoError &error) {
                    if (!ok) {
                        LOG_AT("SUBMISSION", err, "commit body failed user_id={} reason={}",
                               self->request.user_id, error.message);
                        self->fail(Status::Unavailable, 451, "storage");
                        return;
                    }
                    // Keep writer alive through provider completion, even if another worker enqueues immediately.
                    self->body_committed = writer->committed();
                    self->service->workers_->post([self] { self->guard([&] { self->enqueue(); }); });
                });
            });
        });
    }
    void enqueue() {
        parse_mime_tree(request.raw, data->mime_root);
        if (!save_mime_tree(data->body_path, data->mime_root))
            LOG_AT("SUBMISSION", warn, "MIME sidecar save failed mail_id={}", data->id);
        auto path = data->body_path;
        request.raw.clear();
        writer.reset();
        admission_started = true;
        auto admitted = service->queue_->submit_owned_mail(std::move(data));
        if (!admitted.accepted) {
            storage::IoError error;
            if (!storage->remove_object(path, error))
                LOG_AT("SUBMISSION", err, "rejected body cleanup failed path={}", path);
            storage->remove_object(path + ".mime", error);
            fail(Status::Unavailable, 451, "queue");
            return;
        }
        auto self = shared_from_this();
        admitted.ticket.async_wait(
            service->executors_(),
            std::chrono::milliseconds(service->config_.inbound_persist_wait_timeout_ms),
            [self](persist_storage::PersistResult r) {
                if (r.timed_out) {
                    self->fail(Status::Unknown, 0, "persist");
                    return;
                }
                if (r.status != persist_storage::PersistStatus::SUCCESS) {
                    self->fail(Status::Unavailable, 451, "persist");
                    return;
                }
                auto cb = std::move(self->complete);
                cb({Status::Accepted, 250, "persist"});
            });
    }
};

MailSubmissionService::MailSubmissionService(std::shared_ptr<router::IShardRouter> router,
                                             std::shared_ptr<pr::ThreadPoolBase> workers,
                                             std::shared_ptr<AccountPolicy> policy,
                                             std::shared_ptr<persist_storage::PersistentQueue> queue,
                                             ExecutorProvider executors, ServerConfig config,
                                             uint16_t node_id)
    : router_(std::move(router)), workers_(std::move(workers)), policy_(std::move(policy)),
      queue_(std::move(queue)), executors_(std::move(executors)), config_(std::move(config)),
      ids_(node_id % 32, node_id / 32) {
    if (!router_ || !workers_ || !policy_ || !queue_ || !executors_ || node_id > 1023)
        throw std::invalid_argument(
            "MailSubmissionService requires router, workers, policy, queue, executors and node ID 0..1023");
}

void MailSubmissionService::submit_async(Request request, Complete cb) {
    auto op = std::make_shared<Operation>();
    op->service = shared_from_this();
    op->request = std::move(request);
    op->complete = std::move(cb);
    workers_->post([op] {
        op->guard([&] {
            auto &s = *op->service;
            // Defend the trusted boundary against stale/disabled/deleted actors too.
            op->shard = s.router_->route(op->request.sender);
            if (op->shard < 0 || !op->request.user_id) {
                op->fail(Status::AuthFailed, 535, "auth");
                return;
            }
            auto pool = s.router_->get_db_pool(static_cast<size_t>(op->shard));
            auto conn = pool ? pool->acquire_connection() : ScopedConnection::invalid();
            if (!conn->is_valid()) {
                op->fail(Status::Unavailable, 451, "auth");
                return;
            }
            auto user = (*conn)->query(db::sql::build_auth_user_query(), {op->request.sender});
            conn.reset();
            if (!user) {
                op->fail(Status::Unavailable, 451, "auth");
                return;
            }
            if (!user->get_row_count() || safe_stoull(user->get_value(0, "id")) != op->request.user_id ||
                safe_stoull(user->get_value(0, "status")) != 1) {
                op->fail(Status::AuthFailed, 535, "auth");
                return;
            }
            if (op->request.recipients.empty() || op->request.recipients.size() > 100 ||
                op->request.raw.size() > 1024 * 1024) {
                op->fail(Status::Rejected, 552, "message");
                return;
            }
            s.policy_->check_send_quota_async(op->request.sender, s.config_.smtp_daily_send_limit,
                                              [op](bool allowed) {
                                                  if (!allowed) {
                                                      op->fail(Status::Rejected, 550, "mail");
                                                      return;
                                                  }
                                                  op->expand_next();
                                              });
        });
    });
}
} // namespace mail_system::submission
