#include "framework/node/node_trust.h"
#include <stdexcept>
#include <utility>

namespace pr::node {
namespace {
bool valid_key(const std::string& key) { return key.size() == 32; }
} // namespace

TrustStore::TrustStore(std::string controller_public_key, Snapshot initial)
    : controller_public_key_(std::move(controller_public_key)), snapshot_(std::move(initial)) {
    if (!valid_key(controller_public_key_)) throw std::invalid_argument("controller key must be 32 raw bytes");
    for (const auto& neighbor : snapshot_.neighbors)
        if (neighbor.node_id.empty() || !valid_key(neighbor.public_key))
            throw std::invalid_argument("malformed neighbor entry");
}

std::optional<TrustStore::Snapshot> TrustStore::apply(Snapshot next) {
    std::vector<Listener> to_notify;
    std::optional<Snapshot> applied;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (next.epoch <= snapshot_.epoch) return std::nullopt;
        for (const auto& neighbor : next.neighbors)
            if (neighbor.node_id.empty() || !valid_key(neighbor.public_key))
                throw std::invalid_argument("malformed neighbor entry");
        snapshot_ = next;
        applied = std::move(next);
        to_notify = listeners_;
    }
    for (const auto& listener : to_notify) if (listener) listener(*applied);
    return applied;
}

void TrustStore::add_listener(Listener listener) {
    if (!listener) return;
    std::lock_guard<std::mutex> lock(mutex_);
    listeners_.push_back(std::move(listener));
}
} // namespace pr::node
