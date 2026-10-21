#pragma once
#include "mail_system/back/repository/account_repository.h"
#include <cassert>
#include <memory>
#include <string>
#include <utility>

namespace mail_system::test {
struct FakeCredentialAccess final : repository::IAccountCredentialAccess {
    int account_shard = 3, reads = 0, upgrades = 0, touches = 0;
    std::string expected_hash, replacement_hash;
    ReadComplete read_complete;
    Complete upgrade_complete, touch_complete;
    int shard() const override { return account_shard; }
    void read_async(ReadComplete cb) override { assert(!read_complete); ++reads; read_complete = std::move(cb); }
    void upgrade_legacy_hash_async(const std::string& expected, const std::string& replacement, Complete cb) override {
        ++upgrades; expected_hash = expected; replacement_hash = replacement; upgrade_complete = std::move(cb);
    }
    void touch_login_async(Complete cb) override { ++touches; touch_complete = std::move(cb); }
    void finish_read(repository::CredentialLookup result) { auto cb = std::move(read_complete); assert(cb); cb(std::move(result)); }
    void finish_upgrade(bool ok) { auto cb = std::move(upgrade_complete); assert(cb); cb(ok); }
    void finish_touch(bool ok) { auto cb = std::move(touch_complete); assert(cb); cb(ok); }
};
struct FakeAccountRepository final : repository::IAccountRepository {
    int route = 3, opens = 0, recipient_calls = 0, alias_calls = 0, quota_calls = 0, actor_calls = 0;
    int opened_shard = -1, actor_shard = -1, quota_limit = 0;
    uint64_t actor_user = 0;
    std::string opened_address, recipient_address, alias_address, quota_address, actor_address;
    std::shared_ptr<FakeCredentialAccess> access = std::make_shared<FakeCredentialAccess>();
    repository::RecipientLookup recipient_result{repository::AccountLookupStatus::not_found, 0};
    repository::AliasLookup alias_result{true, {}};
    repository::QuotaOutcome quota_result = repository::QuotaOutcome::consumed;
    repository::ActorStatus actor_result = repository::ActorStatus::valid;
    bool defer_recipient = false, defer_alias = false, defer_quota = false;
    std::function<void(repository::RecipientLookup)> recipient_complete;
    std::function<void(repository::AliasLookup)> alias_complete;
    std::function<void(repository::QuotaOutcome)> quota_complete;
    int route_address(const std::string&) override { return route; }
    std::shared_ptr<repository::IAccountCredentialAccess> open_credentials(const std::string& address, int shard) override {
        ++opens; opened_address = address; opened_shard = shard; return access;
    }
    void recipient_async(const std::string& address, std::function<void(repository::RecipientLookup)> cb) override {
        ++recipient_calls; recipient_address = address;
        if (defer_recipient) recipient_complete = std::move(cb); else cb(recipient_result);
    }
    void aliases_async(const std::string& address, std::function<void(repository::AliasLookup)> cb) override {
        ++alias_calls; alias_address = address;
        if (defer_alias) alias_complete = std::move(cb); else cb(alias_result);
    }
    void consume_quota_async(const std::string& address, int limit, std::function<void(repository::QuotaOutcome)> cb) override {
        ++quota_calls; quota_address = address; quota_limit = limit;
        if (defer_quota) quota_complete = std::move(cb); else cb(quota_result);
    }
    repository::ActorStatus verify_actor(const std::string& address, uint64_t id, int shard) override {
        ++actor_calls; actor_address = address; actor_user = id; actor_shard = shard; return actor_result;
    }
};
} // namespace mail_system::test
