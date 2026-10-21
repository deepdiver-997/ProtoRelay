#pragma once
#include "framework/db/db_pool.h"
#include "framework/net/executor.h"
#include "framework/thread_pool/thread_pool_base.h"
#include <atomic>
#include <stdexcept>
#include <utility>

namespace pr {
// Explicit adapter for synchronous drivers. Create in shared_ptr; drain workers
// and completions before stopping the runtime. Calls on a connection stay single-
// flight, and the caller keeps its ScopedConnection through the completion chain.
class AsyncDBConnectionAdapter final : public IDBConnection,
                                       public std::enable_shared_from_this<AsyncDBConnectionAdapter> {
public:
    AsyncDBConnectionAdapter(std::shared_ptr<IDBConnection> inner,
                             std::shared_ptr<ThreadPoolBase> workers,
                             std::shared_ptr<net::IExecutor> completion)
        : inner_(std::move(inner)), workers_(std::move(workers)), completion_(std::move(completion)) {
        if (!inner_ || !workers_ || !completion_)
            throw std::invalid_argument("AsyncDBConnectionAdapter requires driver, workers and executor");
    }
    Dialect dialect() const override { return inner_->dialect(); }
    bool connect() override { return inner_->connect(); }
    void disconnect() override { inner_->disconnect(); }
    bool is_connected() const override { return inner_->is_connected(); }
    std::shared_ptr<IDBResult> query(const std::string& sql) override { return inner_->query(sql); }
    std::shared_ptr<IDBResult> query(const std::string& sql, const std::vector<std::string>& params) override {
        return inner_->query(sql, params);
    }
    bool execute(const std::string& sql) override { return inner_->execute(sql); }
    bool execute(const std::string& sql, const std::vector<std::string>& params) override {
        return inner_->execute(sql, params);
    }
    bool begin_transaction() override { return inner_->begin_transaction(); }
    bool commit() override { return inner_->commit(); }
    bool rollback() override { return inner_->rollback(); }
    std::string get_last_error() const override { return inner_->get_last_error(); }
    std::string escape_string(const std::string& text) const override { return inner_->escape_string(text); }
    bool ping() override { return inner_->ping(); }
    bool async_in_flight() const override { return busy_.load(std::memory_order_acquire) || inner_->async_in_flight(); }

    void async_query(const std::string& sql, QueryCallback cb) override {
        dispatch<std::shared_ptr<IDBResult>>([inner = inner_, sql] { return inner->query(sql); }, std::move(cb));
    }
    void async_query(const std::string& sql, const std::vector<std::string>& params, QueryCallback cb) override {
        dispatch<std::shared_ptr<IDBResult>>([inner = inner_, sql, params] { return inner->query(sql, params); }, std::move(cb));
    }
    void async_execute(const std::string& sql, ExecuteCallback cb) override {
        dispatch<bool>([inner = inner_, sql] { return inner->execute(sql); }, std::move(cb));
    }
    void async_execute(const std::string& sql, const std::vector<std::string>& params, ExecuteCallback cb) override {
        dispatch<bool>([inner = inner_, sql, params] { return inner->execute(sql, params); }, std::move(cb));
    }
    void async_begin_transaction(ExecuteCallback cb) override {
        dispatch<bool>([inner = inner_] { return inner->begin_transaction(); }, std::move(cb));
    }
    void async_commit(ExecuteCallback cb) override {
        dispatch<bool>([inner = inner_] { return inner->commit(); }, std::move(cb));
    }
    void async_rollback(ExecuteCallback cb) override {
        dispatch<bool>([inner = inner_] { return inner->rollback(); }, std::move(cb));
    }
private:
    template<class Result, class Work, class Callback>
    void dispatch(Work work, Callback callback) {
        auto self = shared_from_this();
        auto cb = std::make_shared<Callback>(std::move(callback));
        bool expected = false;
        if (!busy_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            completion_->post([cb] { if (*cb) (*cb)(Result{}); });
            return;
        }
        try {
            workers_->post([self, work = std::move(work), cb]() mutable {
                Result result{};
                try { result = work(); } catch (...) {} // same null/false failure shape as drivers
                self->completion_->post([self, cb, result = std::move(result)]() mutable {
                    self->busy_.store(false, std::memory_order_release);
                    if (*cb) (*cb)(std::move(result));
                });
            });
        } catch (...) {
            busy_.store(false, std::memory_order_release);
            completion_->post([cb] { if (*cb) (*cb)(Result{}); });
        }
    }
    std::shared_ptr<IDBConnection> inner_;
    std::shared_ptr<ThreadPoolBase> workers_;
    std::shared_ptr<net::IExecutor> completion_;
    std::atomic<bool> busy_{false};
};

// Opt-in pool factory decoration; a synchronous driver remains unchanged unless
// the application explicitly constructs this service at its composition root.
class AsyncDBServiceAdapter final : public DBService {
public:
    AsyncDBServiceAdapter(std::shared_ptr<DBService> inner, std::shared_ptr<ThreadPoolBase> workers,
                          std::function<std::shared_ptr<net::IExecutor>()> completions)
        : inner_(std::move(inner)), workers_(std::move(workers)), completions_(std::move(completions)) {
        if (!inner_ || !workers_ || !completions_)
            throw std::invalid_argument("AsyncDBServiceAdapter requires service, workers and executors");
    }
    std::shared_ptr<IDBConnection> create_connection(const std::string& host, const std::string& user,
        const std::string& password, const std::string& database, unsigned int port) override {
        auto connection = inner_->create_connection(host, user, password, database, port);
        if (!connection) return {};
        return std::make_shared<AsyncDBConnectionAdapter>(std::move(connection), workers_, completions_());
    }
    std::string get_service_name() const override { return inner_->get_service_name() + "+worker"; }
private:
    std::shared_ptr<DBService> inner_;
    std::shared_ptr<ThreadPoolBase> workers_;
    std::function<std::shared_ptr<net::IExecutor>()> completions_;
};
}
