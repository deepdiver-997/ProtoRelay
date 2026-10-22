#include "mail_system/back/persist_storage/persistent_queue.h"
#include "framework/metrics_server.h"
#include "framework/storage/io_error.h"
#include "mail_system/back/mailServer/outbound/mx_routing_utils.h"
#include "mail_system/back/mailServer/outbound_server.h"
#include "mail_system/back/repository/persistence_repository.h"
#include "mail_system/back/router/i_shard_router.h"
#include "framework/thread_pool/thread_pool_base.h"
#include "mail_system/back/common/logger.h"
#include <boost/lockfree/queue.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <optional>
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#ifdef __linux__
#include <sys/sysinfo.h>
#endif

namespace mail_system {
namespace persist_storage {

// Keep admission capacity and immediate failure identical while hiding Boost.
struct PersistentQueue::QueueItem {
    std::unique_ptr<mail> mail_data;
    PersistSubmissionTicket ticket;
};
struct PersistentQueue::TaskQueue {
    boost::lockfree::queue<QueueItem*, boost::lockfree::capacity<16384>> entries;
    bool push(QueueItem* item) { return entries.push(item); }
    bool pop(QueueItem*& item) { return entries.pop(item); }
};
namespace {
std::optional<std::uint64_t> query_available_memory_bytes() {
#ifdef __APPLE__
    mach_port_t host_port = mach_host_self();
    vm_statistics64_data_t vm_stats{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(host_port, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm_stats), &count) != KERN_SUCCESS) {
        return std::nullopt;
    }

    const auto page_size = static_cast<std::uint64_t>(vm_kernel_page_size);
    return static_cast<std::uint64_t>(vm_stats.free_count + vm_stats.inactive_count) * page_size;
#elif defined(__linux__)
    struct sysinfo info {};
    if (sysinfo(&info) != 0) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(info.freeram) * static_cast<std::uint64_t>(info.mem_unit);
#else
    return std::nullopt;
#endif
}

} // namespace

PersistentQueue::PersistentQueue(
    std::shared_ptr<router::IShardRouter> shard_router,
    std::shared_ptr<pr::ThreadPoolBase> worker_pool,
    std::shared_ptr<repository::IPersistenceRepository> persistence_repository
) : m_shardRouter(std::move(shard_router)),
    worker_pool_(std::move(worker_pool)),
    repository_(persistence_repository ? std::move(persistence_repository) : repository::make_sql_persistence_repository(m_shardRouter)),
    task_queue_(std::make_unique<TaskQueue>()),
    shutdown_(false) {
    // 启动工作线程
    worker_thread_ = std::thread(&PersistentQueue::worker_loop, this);
    LOG_PERSISTENT_QUEUE_INFO("PersistentQueue initialized and worker thread started");
}

int PersistentQueue::shard_from_mail(const mail* m) const {
    if (!m_shardRouter || !m) return 0;
    const std::string& key = !m->from.empty() ? m->from :
                             (!m->to.empty() ? m->to.front() : "");
    if (key.empty()) return 0;
    int shard = m_shardRouter->route(key);
    return shard >= 0 ? shard : 0;
}

PersistentQueue::~PersistentQueue() {
    shutdown();
    if (worker_thread_.joinable()) worker_thread_.join();
    std::unique_lock<std::mutex> lock(submission_mutex_);
    drained_.wait(lock, [this] { return inflight_mail_count_.load() == 0; });
}

SubmitOwnedMailResult PersistentQueue::submit_owned_mail(std::unique_ptr<mail> mail_data) {
    SubmitOwnedMailResult result;
    result.rejected_mail = std::move(mail_data);
    if (!result.rejected_mail) {
        result.error = "mail is null";
        LOG_PERSISTENT_QUEUE_ERROR("Cannot submit null mail"); return result;
    }
    {
        std::lock_guard<std::mutex> lock(submission_mutex_);
        if (is_shutdown()) result.error = "persistent queue is shutdown";
        else if (result.rejected_mail->persist_status != PersistStatus::PENDING)
            result.error = "mail is not pending";
        else should_reject_submission(*result.rejected_mail, result.error);
        if (!result.error.empty()) {
            LOG_PERSISTENT_QUEUE_WARN("Rejected mail_id={} reason={}", result.rejected_mail->id, result.error);
            return result;
        }
        result.ticket.mail_id = result.rejected_mail->id;
        result.ticket.status = result.rejected_mail->persist_status;
        auto item = std::make_unique<QueueItem>(QueueItem{std::move(result.rejected_mail), result.ticket});
        // 先预占计数再发布给消费者；满队列直接背压，不自旋等待消费者。
        queued_task_count_.fetch_add(1);
        inflight_mail_count_.fetch_add(1);
        if (!task_queue_->push(item.get())) {
            queued_task_count_.fetch_sub(1); inflight_mail_count_.fetch_sub(1);
            result.rejected_mail = std::move(item->mail_data);
            result.error = "persistent queue is full";
            LOG_PERSISTENT_QUEUE_WARN("Rejected mail_id={} reason={}", result.ticket.mail_id, result.error);
            result.ticket = {};
            return result;
        }
        item.release();
    }
    push_queue_metrics();
    available_.notify_one();
    result.accepted = true;
    LOG_PERSISTENT_QUEUE_INFO("Mail admitted mail_id={} queued={} inflight={}",
        result.ticket.mail_id, queue_size(), inflight_count());
    return result;
}

bool PersistentQueue::submit_mails(std::vector<mail*>& mail_list) {
    LOG_PERSISTENT_QUEUE_WARN("submit_mails(raw pointers) is deprecated and disabled for ownership safety");
    return false;
}

void PersistentQueue::delete_task(const mail& mail_data) {
    // 调用线程快照分片与 id：投递后的工作阶段只持有仓库句柄和普通值，
    // 不解引用调用方（session）的 mail，消除 mail 先释放、任务后执行的竞态。
    const int shard = shard_from_mail(&mail_data);
    const auto mail_id = mail_data.id;
    std::shared_ptr<repository::IPersistenceRepository> persistence_repository;
    { std::lock_guard<std::mutex> lock(submission_mutex_); persistence_repository = repository_; }
    auto erase_metadata = [persistence_repository, shard, mail_id]() {
        try {
            const auto result = persistence_repository->erase_metadata({shard, {mail_id}, false});
            if (!result.ok) LOG_PERSISTENT_QUEUE_WARN("Metadata delete failed mail_id={}: {}", mail_id, result.error);
        } catch (const std::exception& error) {
            LOG_PERSISTENT_QUEUE_ERROR("Metadata delete exception mail_id={}: {}", mail_id, error.what());
        }
    };
    try {
        if (!worker_pool_) throw std::runtime_error("Persistence Worker is unavailable");
        worker_pool_->post(std::move(erase_metadata));
    } catch (const std::exception& error) {
        LOG_PERSISTENT_QUEUE_WARN("Deferred metadata delete not scheduled mail_id={}: {}", mail_id, error.what());
    }
}

void PersistentQueue::delete_multi_tasks(std::vector<mail*>& mail_list) {
    if (mail_list.empty()) return;
    repository::PersistenceEraseRequest request;
    request.shard = shard_from_mail(mail_list.front());
    for (const auto* message : mail_list) if (message) request.mail_ids.push_back(message->id);
    std::shared_ptr<repository::IPersistenceRepository> persistence_repository;
    { std::lock_guard<std::mutex> lock(submission_mutex_); persistence_repository = repository_; }
    try {
        const auto result = persistence_repository->erase_metadata(request);
        if (!result.ok) LOG_PERSISTENT_QUEUE_WARN("Batch metadata delete failed: {}", result.error);
    } catch (const std::exception& error) { LOG_PERSISTENT_QUEUE_ERROR("Batch metadata delete exception: {}", error.what()); }
}

size_t PersistentQueue::queue_size() const {
    return queued_task_count_.load(std::memory_order_relaxed);
}

void PersistentQueue::set_outbound_server(std::shared_ptr<mail_system::outbound::OutboundServer> server) {
    std::lock_guard<std::mutex> lock(submission_mutex_);
    outbound_server_ = std::move(server);
}

void PersistentQueue::set_local_domain(std::string local_domain) {
    std::lock_guard<std::mutex> lock(submission_mutex_);
    if (!local_domain.empty()) {
        local_domain_ = std::move(local_domain);
    }
}

void PersistentQueue::set_pressure_config(PersistentQueuePressureConfig config) {
    std::lock_guard<std::mutex> lock(submission_mutex_);
    pressure_config_ = config;
}

void PersistentQueue::set_max_attempts(size_t max_attempts) {
    std::lock_guard<std::mutex> lock(submission_mutex_);
    max_attempts_ = max_attempts;
}

void PersistentQueue::set_batch_pop_size(size_t batch_size) {
    batch_pop_size_.store(batch_size);
}
void PersistentQueue::set_persistence_repository(std::shared_ptr<repository::IPersistenceRepository> persistence_repository) {
    if (!persistence_repository) throw std::invalid_argument("Persistence repository is required");
    std::lock_guard<std::mutex> lock(submission_mutex_); repository_ = std::move(persistence_repository);
}

void PersistentQueue::shutdown() {
    { std::lock_guard<std::mutex> lock(submission_mutex_); shutdown_.store(true, std::memory_order_release); }
    available_.notify_all();
}

void PersistentQueue::finish_task() {
    std::lock_guard<std::mutex> lock(submission_mutex_);
    inflight_mail_count_.fetch_sub(1);
    push_queue_metrics();
    // 析构等待者拿到锁之后，本函数不再访问 queue 的成员。
    drained_.notify_all();
}

bool PersistentQueue::process_task() {
    QueueItem* raw = nullptr;
    std::shared_ptr<repository::IPersistenceRepository> persistence_repository;
    std::shared_ptr<outbound::OutboundServer> outbound_snapshot;
    repository::PersistenceOptions options;
    {
        std::unique_lock<std::mutex> lock(submission_mutex_);
        available_.wait(lock, [this] { return is_shutdown() || queued_task_count_.load() > 0; });
        if (!task_queue_->pop(raw)) return false;
        queued_task_count_.fetch_sub(1);
        persistence_repository = repository_; outbound_snapshot = outbound_server_;
        options.local_domain = local_domain_; options.max_attempts = max_attempts_;
    }
    std::unique_ptr<QueueItem> item(raw);
    auto ticket = item->ticket;
    auto message = std::shared_ptr<mail>(std::move(item->mail_data));
    push_queue_metrics();
    auto once = std::make_shared<std::atomic<bool>>(false);
    auto finish = [this, ticket, message, once](PersistStatus status, std::string error = {}) {
        if (once->exchange(true)) return;
        try { ticket.complete(status, std::move(error), message && message->deduplicated_inbound); }
        catch (...) { LOG_PERSISTENT_QUEUE_ERROR("Cannot notify persistence observer mail_id={}", ticket.mail_id); }
        finish_task(); // No further queue access: its destructor may now proceed.
    };
    auto remove_files = [this, message] {
        try { if (message) cleanup_mail_files(message.get()); }
        catch (...) { LOG_PERSISTENT_QUEUE_WARN("File cleanup exception mail_id={}", message ? message->id : 0); }
    };
    if (!message || is_shutdown()) {
        remove_files(); finish(PersistStatus::CANCELLED, "queue shutdown before persistence started"); return true;
    }
    const repository::PersistenceRequest request{message, std::move(options)};
    auto received = std::make_shared<std::atomic<bool>>(false);
    auto complete = [this, request, message, persistence_repository, outbound_snapshot, finish, remove_files, received](repository::PersistenceResult result) mutable {
        if (received->exchange(true)) return;
        const bool success = result.success();
        const bool duplicate = result.disposition == repository::PersistenceDisposition::deduplicated;
        message->deduplicated_inbound = duplicate;
        try {
            auto finalize = [this, request, message, persistence_repository, outbound_snapshot, result = std::move(result), finish, remove_files]() mutable {
                if (!result.success()) {
                    try { cleanup_failed_mail(message.get(), persistence_repository); }
                    catch (...) { remove_files(); }
                    finish(PersistStatus::FAILED, std::move(result.error)); return;
                }
                // Post-commit failures cannot trigger failed-mail cleanup, revoke a
                // successful ticket, or delete files retained by committed rows.
                try {
                    if (result.disposition == repository::PersistenceDisposition::deduplicated) remove_files();
                    else if (result.disposition == repository::PersistenceDisposition::committed) {
                        try {
                            for (const auto& copy : persistence_repository->persist_recipient_copies(request, result.source_shard))
                                if (!copy.ok) LOG_PERSISTENT_QUEUE_WARN("Recipient copy failed mail_id={} recipient={}: {}", message->id, copy.recipient, copy.error);
                        } catch (...) { LOG_PERSISTENT_QUEUE_WARN("Post-commit recipient copies failed mail_id={}", message->id); }
                        if (outbound_snapshot && outbound::has_external_recipient(*message, request.options.local_domain)) for (const auto& record : result.outbox_records) {
                            auto task = std::make_unique<outbound::MailDeliveryTask>();
                            task->mail_id = record.mail_id; task->record_id = record.id;
                            task->sender = record.sender; task->recipient = record.recipient;
                            task->mail_ptr = std::make_shared<mail>(*message);
                            task->attempt_count = record.attempt_count; task->max_attempts = record.max_attempts;
                            outbound_snapshot->submit(std::move(task));
                        }
                    }
                } catch (...) { LOG_PERSISTENT_QUEUE_WARN("Post-commit handoff failed mail_id={}", message->id); }
                finish(PersistStatus::SUCCESS);
            };
            // A repository callback still owns its transaction lease. Queueing this
            // stage lets it return before cleanup/recipient work borrows the pool.
            worker_pool_->post(std::move(finalize));
        } catch (...) {
            if (!success || duplicate) remove_files();
            finish(success ? PersistStatus::SUCCESS : PersistStatus::FAILED, success ? "" : "Cannot schedule persistence cleanup");
        }
    };
    try {
        if (!worker_pool_) throw std::runtime_error("Persistence Worker is unavailable");
        worker_pool_->post([this, ticket, request, message, persistence_repository, complete, finish, remove_files]() mutable {
            if (ticket.is_cancel_requested() || is_shutdown()) {
                remove_files(); finish(PersistStatus::CANCELLED, "cancelled before persistence started"); return;
            }
            message->persist_status = PersistStatus::PROCESSING;
            try { persistence_repository->persist_async(request, complete); }
            catch (const std::exception& error) { repository::PersistenceResult result; result.error = error.what(); complete(std::move(result)); }
            catch (...) { repository::PersistenceResult result; result.error = "Persistence repository exception"; complete(std::move(result)); }
        });
    } catch (...) { remove_files(); finish(PersistStatus::FAILED, "Cannot schedule persistence"); }
    return true;
}

void PersistentQueue::worker_loop() {
    while (process_task()) {}
}

void PersistentQueue::cleanup_mail_files(mail* mail_data) {
    if (!mail_data) {
        return;
    }

    if (!mail_data->body_path.empty()) {
        if (m_shardRouter->get_storage(0)) {
            pr::IoError error;
            if (!m_shardRouter->get_storage(0)->remove_object(mail_data->body_path, error)) {
                LOG_PERSISTENT_QUEUE_WARN("Failed to delete mail body object: {}, error={}",
                                          mail_data->body_path,
                                          error.message);
            }
        } else if (std::remove(mail_data->body_path.c_str()) != 0) {
            LOG_PERSISTENT_QUEUE_WARN("Failed to delete mail body file: {}", mail_data->body_path);
        }
    }

    for (const auto& attachment : mail_data->attachments) {
        if (attachment.filepath.empty()) {
            continue;
        }
        if (m_shardRouter->get_storage(0)) {
            pr::IoError error;
            if (!m_shardRouter->get_storage(0)->remove_object(attachment.filepath, error)) {
                LOG_PERSISTENT_QUEUE_WARN("Failed to delete attachment object: {}, error={}",
                                          attachment.filepath,
                                          error.message);
            }
        } else if (std::remove(attachment.filepath.c_str()) != 0) {
            LOG_PERSISTENT_QUEUE_WARN("Failed to delete attachment file: {}", attachment.filepath);
        }
    }
}

void PersistentQueue::cleanup_failed_mail(mail* mail_data, const std::shared_ptr<repository::IPersistenceRepository>& persistence_repository) {
    if (!mail_data) return;
    try {
        const auto result = persistence_repository->erase_metadata({shard_from_mail(mail_data), {mail_data->id}, !mail_data->attachments.empty()});
        if (!result.ok) LOG_PERSISTENT_QUEUE_WARN("Failed metadata cleanup mail_id={}: {}", mail_data->id, result.error);
    } catch (...) { LOG_PERSISTENT_QUEUE_WARN("Metadata cleanup exception mail_id={}", mail_data->id); }
    cleanup_mail_files(mail_data);
}

bool PersistentQueue::should_reject_submission(const mail& mail_data, std::string& reason) {
    (void)mail_data;
    if (pressure_config_.max_inflight_mails > 0 &&
        inflight_mail_count_.load(std::memory_order_relaxed) >= pressure_config_.max_inflight_mails) {
        reason = "persist inflight limit reached";
        return true;
    }

    if (is_db_under_pressure(reason)) {
        return true;
    }

    if (is_memory_under_pressure(reason)) {
        return true;
    }

    return false;
}

bool PersistentQueue::is_db_under_pressure(std::string& reason) const {
    if (pressure_config_.min_db_available_connections == 0) return false;
    auto db = m_shardRouter ? m_shardRouter->get_db_pool(0) : nullptr;
    if (!db) return false;

    const auto available = db->get_available_connections();
    if (available >= pressure_config_.min_db_available_connections) {
        return false;
    }

    reason = "database available connections below threshold";
    return true;
}

bool PersistentQueue::is_memory_under_pressure(std::string& reason) const {
    if (pressure_config_.min_available_memory_mb == 0) {
        return false;
    }

    const auto available_bytes = query_available_memory_bytes();
    if (!available_bytes.has_value()) {
        return false;
    }

    const auto threshold_bytes = static_cast<std::uint64_t>(pressure_config_.min_available_memory_mb) * 1024ULL * 1024ULL;
    if (available_bytes.value() >= threshold_bytes) {
        return false;
    }

    reason = "available memory below threshold";
    return true;
}

void PersistentQueue::push_queue_metrics() {
    if (auto m = metrics_.lock()) {
        m->set_gauge("protorelay_queue_inflight", {}, inflight_mail_count_.load());
        m->set_gauge("protorelay_queue_depth", {}, queued_task_count_.load());
    }
}

} // namespace persist_storage
} // namespace mail_system
