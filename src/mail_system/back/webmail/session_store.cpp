#include "mail_system/back/webmail/session_store.h"
#include "framework/log/log_macros.h"
#include <exception>
#include <utility>

namespace mail_system::webmail {
namespace {
void run(const std::shared_ptr<pr::ThreadPoolBase>& workers, std::function<void()> task) {
    if (workers) workers->post(std::move(task)); else task();
}
} // namespace
void SessionStore::put_async(std::string token, WebSessionInfo user, SessionPasswordPtr password,
                             std::function<void(bool)> cb) {
    if (!database_) {
        put_memory(token, std::move(user), std::move(password));
        cb(true);
        return;
    }
    auto repository = repository_;
    const auto ttl = ttl_;
    // Passwords are intentionally not captured by persistent tasks.
    run(workers_, [repository, ttl, token = std::move(token), user = std::move(user), cb = std::move(cb)] {
        bool ok = false;
        try { ok = repository->put(token, user, ttl); }
        catch (const std::exception& error) { LOG_AT("WEBAPI", err, "session put repository exception: {}", error.what()); }
        cb(ok);
    });
}
void SessionStore::lookup_async(std::string token, std::function<void(LookupResult)> cb) const {
    if (!database_) {
        LookupResult out;
        out.found = lookup(token, out.user, &out.password);
        cb(std::move(out));
        return;
    }
    if (!valid_web_session_token(token)) { cb({}); return; }
    auto repository = repository_;
    run(workers_, [repository, token = std::move(token), cb = std::move(cb)] {
        LookupResult out;
        try {
            auto stored = repository->lookup(token);
            out.found = stored.found;
            out.server_error = stored.server_error;
            out.user = std::move(stored.user);
        } catch (const std::exception& error) {
            LOG_AT("WEBAPI", err, "session lookup repository exception: {}", error.what());
            out.server_error = true;
        }
        cb(std::move(out));
    });
}
void SessionStore::erase_async(std::string token, std::function<void(bool)> cb) {
    if (!database_) { erase(token); cb(true); return; }
    if (!valid_web_session_token(token)) { cb(true); return; }
    auto repository = repository_;
    run(workers_, [repository, token = std::move(token), cb = std::move(cb)] {
        bool ok = false;
        try { ok = repository->erase(token); }
        catch (const std::exception& error) { LOG_AT("WEBAPI", err, "session erase repository exception: {}", error.what()); }
        cb(ok);
    });
}
} // namespace mail_system::webmail
