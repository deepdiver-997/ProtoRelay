#include "mail_system/back/repository/pop3_lock_repository.h"
#include "mail_system/back/common/safe_parse.h"
#include "pop3_repository_completion.h"
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mail_system::repository {
namespace {
using LockCompletion = detail::Pop3Completion<Pop3LockStatus>;
void verify(Pop3DbContext context, const Pop3LockKey& key, std::shared_ptr<LockCompletion> done) {
    if (!context || !context->valid()) { done->finish(Pop3LockStatus::database_error); return; }
    try {
        (*context->lease)->async_query("SELECT COUNT(*) as cnt FROM pop3_session_lock WHERE user_id = ? AND session_id = ?",
            {std::to_string(key.user_id), key.session_id},
            [context, done](std::shared_ptr<pr::IDBResult> rows) {
                auto status = Pop3LockStatus::database_error;
                try {
                    uint64_t count = 0;
                    if (rows && rows->get_row_count() && parse_uint(rows->get_value(0, "cnt"), count))
                        status = count ? Pop3LockStatus::owned : Pop3LockStatus::not_owned;
                } catch (...) {}
                done->finish(status);
            });
    } catch (...) { done->finish(Pop3LockStatus::database_error); }
}
class SqlPop3LockRepository final : public IPop3LockRepository {
public:
    void acquire_async(Pop3DbContext context, Pop3LockKey key, Complete complete) override {
        auto done = std::make_shared<LockCompletion>(std::move(complete));
        if (!context || !context->valid()) { done->finish(Pop3LockStatus::database_error); return; }
        try {
            const std::vector<std::string> params{std::to_string(key.user_id), key.session_id};
            (*context->lease)->async_execute(
                "INSERT INTO pop3_session_lock (user_id, session_id, acquired_at, last_heartbeat) VALUES (?, ?, NOW(), NOW()) "
                "ON DUPLICATE KEY UPDATE "
                "session_id = IF(session_id = VALUES(session_id), VALUES(session_id), session_id), "
                "last_heartbeat = IF(session_id = VALUES(session_id), NOW(), last_heartbeat)", params,
                [context, key = std::move(key), done](bool updated) {
                    if (!updated) { done->finish(Pop3LockStatus::database_error); return; }
                    verify(context, key, done);
                });
        } catch (...) { done->finish(Pop3LockStatus::database_error); }
    }
    void verify_async(Pop3DbContext context, Pop3LockKey key, Complete complete) override {
        verify(std::move(context), key, std::make_shared<LockCompletion>(std::move(complete)));
    }
    void renew_async(Pop3DbContext context, Pop3LockKey key, Complete complete) override {
        auto done = std::make_shared<LockCompletion>(std::move(complete));
        if (!context || !context->valid()) { done->finish(Pop3LockStatus::database_error); return; }
        try {
            const std::vector<std::string> params{std::to_string(key.user_id), key.session_id};
            (*context->lease)->async_execute("UPDATE pop3_session_lock SET last_heartbeat = NOW() WHERE user_id = ? AND session_id = ?", params,
                [context, key = std::move(key), done](bool updated) {
                    if (!updated) { done->finish(Pop3LockStatus::database_error); return; }
                    verify(context, key, done);
                });
        } catch (...) { done->finish(Pop3LockStatus::database_error); }
    }
    void release_async(Pop3DbContext context, Pop3LockKey key, ReleaseComplete complete) override {
        auto done = std::make_shared<detail::Pop3Completion<bool>>(std::move(complete));
        if (!context || !context->valid()) { done->finish(false); return; }
        try {
            (*context->lease)->async_execute("DELETE FROM pop3_session_lock WHERE user_id = ? AND session_id = ?",
                {std::to_string(key.user_id), key.session_id}, [context, done](bool released) { done->finish(released); });
        } catch (...) { done->finish(false); }
    }
    bool sweep_expired(Pop3DbContext context) override {
        try {
            return context && context->valid() && (*context->lease)->execute(
                "DELETE FROM pop3_session_lock WHERE last_heartbeat < NOW() - INTERVAL 5 MINUTE");
        } catch (...) { return false; }
    }
};
} // namespace
std::shared_ptr<IPop3LockRepository> make_sql_pop3_lock_repository() { return std::make_shared<SqlPop3LockRepository>(); }
} // namespace mail_system::repository
