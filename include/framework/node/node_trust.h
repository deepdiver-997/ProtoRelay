#ifndef PR_FRAMEWORK_NODE_NODE_TRUST_H
#define PR_FRAMEWORK_NODE_NODE_TRUST_H
// Per-node trust set for the node link layer: the neighbor table and config
// epoch from the controller-issued node bundle, plus the controller public
// key that authenticates epoch-config updates. Pure data with a mutex — no
// crypto library types, so OpenSSL-free builds include it freely.
//
// Minimum knowledge: a node's table only ever holds its own neighbors. The
// controller never broadcasts; each node receives (out of band or relayed as
// an epoch-config frame) the update meant for it, verifies the controller
// signature, and applies it here. Epochs are monotonic: stale updates are
// ignored, revoked neighbors are simply absent from the newer snapshot.
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pr::node {

class TrustStore {
public:
    // Neighbor identity: node_id and its raw 32-byte Ed25519 public key.
    struct Neighbor { std::string node_id; std::string public_key; };
    // A full configuration generation. Applying one replaces the neighbor set
    // wholesale; a neighbor missing from `neighbors` is thereby revoked.
    struct Snapshot {
        uint64_t epoch = 0;
        std::vector<Neighbor> neighbors;
        std::vector<std::string> revoked; // informational: ids dropped by this update
    };
    // Fired after every successful apply, outside the store mutex. Listeners
    // may outlive their registrant; they must weakly re-validate before acting.
    using Listener = std::function<void(const Snapshot&)>;

    // controller_public_key is the raw 32-byte Ed25519 key every epoch-config
    // update must be signed with; throws std::invalid_argument when it is not
    // exactly 32 bytes or a neighbor entry is malformed.
    TrustStore(std::string controller_public_key, Snapshot initial);
    TrustStore(const TrustStore&) = delete;
    TrustStore& operator=(const TrustStore&) = delete;

    const std::string& controller_public_key() const { return controller_public_key_; }
    uint64_t epoch() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_.epoch;
    }
    // Raw public key of a trusted neighbor, or empty when unknown/untrusted.
    std::string neighbor_key(const std::string& node_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& neighbor : snapshot_.neighbors)
            if (neighbor.node_id == node_id) return neighbor.public_key;
        return {};
    }
    bool trusts(const std::string& node_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& neighbor : snapshot_.neighbors)
            if (neighbor.node_id == node_id) return true;
        return false;
    }
    // Applies only when next.epoch is strictly greater than the current one;
    // returns the snapshot applied, or nullopt for a stale (ignored) update.
    // Listeners fire after the state change and outside the mutex.
    std::optional<Snapshot> apply(Snapshot next);
    void add_listener(Listener listener);

private:
    std::string controller_public_key_;
    mutable std::mutex mutex_;
    Snapshot snapshot_;
    std::vector<Listener> listeners_;
};

} // namespace pr::node
#endif
