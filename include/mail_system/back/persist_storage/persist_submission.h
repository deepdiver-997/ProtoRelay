#pragma once
#include "mail_system/back/entities/mail.h"
#include <boost/asio/any_io_executor.hpp>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>

namespace mail_system::persist_storage {
struct PersistResult {
    uint64_t mail_id = 0;
    PersistStatus status = PersistStatus::FAILED;
    std::string error;
    bool deduplicated = false;
    // 等待超时不是持久化终态；正在提交的事务仍可能完成。
    bool timed_out = false;
};

// 终态只发布一次；支持完成前/后订阅。通知总是 post 到调用方 executor。
class PersistCompletion {
public:
    using Complete = std::function<void(PersistResult)>;
    bool complete(PersistResult result, const SharedPersistStatus& status);
    void observe(boost::asio::any_io_executor executor, Complete callback);
    std::optional<PersistResult> result() const;
private:
    struct Observer { boost::asio::any_io_executor executor; Complete callback; };
    mutable std::mutex mutex_;
    std::optional<PersistResult> result_;
    std::vector<Observer> observers_;
};

struct PersistSubmissionTicket {
    uint64_t mail_id = 0;
    SharedPersistStatus status;
    std::shared_ptr<std::atomic<bool>> cancel_requested = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<PersistCompletion> completion = std::make_shared<PersistCompletion>();
    bool valid() const { return mail_id != 0; }
    // 只保证尚未开始的任务可以取消，不能撤回已经提交的数据库事务。
    void request_cancel() const { if (cancel_requested) cancel_requested->store(true, std::memory_order_release); }
    bool is_cancel_requested() const { return cancel_requested && cancel_requested->load(std::memory_order_acquire); }
    bool complete(PersistStatus terminal, std::string error = {}, bool deduplicated = false) const;
    void async_wait(boost::asio::any_io_executor executor, std::chrono::milliseconds timeout,
                    PersistCompletion::Complete callback) const;
};
} // namespace mail_system::persist_storage
