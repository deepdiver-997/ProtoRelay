#undef NDEBUG
#include "mail_system/back/common/password_rehash.h"
#include "fake_account_repository.h"
#include <cassert>
#include <iostream>
#include <memory>
#include <string>

using namespace mail_system;
int main() {
    const std::string address = "legacy@example.com";
    // Only verified legacy credentials enter this helper. Persistence SQL/CAS
    // parameters are tested separately at the account repository boundary.
    for (bool persisted : {true, false}) {
        auto access = std::make_shared<test::FakeCredentialAccess>();
        auto cache = std::make_shared<AuthCache>();
        const std::string stored = persisted ? "hello" : "5d41402abc4b2a76b9719d911017c592";
        AuthCacheEntry entry{stored, 1, 42, 3}; cache->store(address, entry);
        int done = 0;
        rehash_password_if_needed_async(access, cache, address, "hello", stored, entry, [&] { ++done; });
        assert(access->upgrades == 1 && access->expected_hash == stored && !done);
        assert(is_bcrypt_hash(access->replacement_hash) && bcrypt_verify("hello", access->replacement_hash));
        AuthCacheEntry current;
        assert(cache->lookup(address, current) && current.password_hash == stored);
        access->finish_upgrade(persisted);
        assert(done == 1 && cache->lookup(address, current));
        assert(current.password_hash == (persisted ? access->replacement_hash : stored));
        assert(current.user_id == 42 && current.shard == 3 && current.status == 1);
    }
    auto access = std::make_shared<test::FakeCredentialAccess>();
    const auto hash = bcrypt_hash("pw", 4);
    int done = 0;
    rehash_password_if_needed_async(access, nullptr, address, "pw", hash, {hash, 1, 1, 0}, [&] { ++done; });
    assert(done == 1 && !access->upgrades); // Bcrypt does not touch persistence.
    rehash_password_if_needed_async(nullptr, nullptr, address, "pw", "pw", {}, [&] { ++done; });
    assert(done == 2);
    rehash_password_if_needed_async(access, nullptr, address, "pw", "pw", {}, [&] { ++done; });
    assert(done == 2 && access->upgrades == 1);
    access->finish_upgrade(true);
    assert(done == 3); // A deployment without a credential cache is supported.
    std::cout << "password_rehash_test: passed\n";
}
