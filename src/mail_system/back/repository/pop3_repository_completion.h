#pragma once
#include <atomic>
#include <functional>
#include <utility>

namespace mail_system::repository::detail {
// A driver may complete inline and then throw. Never invoke the application
// callback twice when converting driver/mapping exceptions into domain failure.
template <typename... Args> struct Pop3Completion {
    explicit Pop3Completion(std::function<void(Args...)> callback) : callback(std::move(callback)) {}
    void finish(Args... args) {
        if (!called.exchange(true) && callback) callback(std::move(args)...);
    }
    std::atomic<bool> called{false};
    std::function<void(Args...)> callback;
};
} // namespace mail_system::repository::detail
