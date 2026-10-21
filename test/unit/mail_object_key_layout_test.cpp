#undef NDEBUG
#include "mail_system/back/storage/mail_object_key_layout.h"
#include "mail_system/back/storage/async_storage_provider.h"
#include "mail_system/back/storage/distributed_file_storage_provider.h"
#include "mail_system/back/storage/local_file_storage_provider.h"
#include "mail_system/back/storage/null_storage_provider.h"
#include "mail_system/back/storage/storage_config.h"
#if PROTORELAY_ENABLE_HDFS_WEB_STORAGE
#include "mail_system/back/storage/hdfs_web_storage_provider.h"
#endif
#ifdef ENABLE_S3_STORAGE
#include "mail_system/back/storage/s3_storage_provider.h"
#endif
#include "framework/storage/i_object_metadata_cache.h"
#include "framework/storage/null_storage_provider.h"
#include "test_temp_dir.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <locale>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(BOOST_ASIO_VERSION) || defined(BOOST_VERSION)
#error "Mail object key layout contracts must not expose an Asio backend"
#endif
namespace {
namespace app = mail_system::storage;
using Kind = app::MailObjectKeyLayout::Kind;
const std::string filename = "a/b\\c d:e*中文.bin";
const std::string sanitized = "a_b_c_d_e_中文.bin";
void legacy_layout_vectors() {
    app::MailObjectKeyLayout local(Kind::local, "/mail", "/attachment/");
    assert(local.build_mail_body_key(42) == "/mail/42");
    assert(local.build_attachment_key(42, filename) == "/attachment/42_1_" + sanitized);
    assert(local.build_attachment_key(7, "") == "/attachment/7_2_attachment");
    app::MailObjectKeyLayout empty(Kind::local), backslash(Kind::local, "root\\", "att\\");
    assert(empty.build_mail_body_key(0) == "0");
    assert(empty.build_attachment_key(0, "") == "0_1_attachment");
    assert(backslash.build_mail_body_key(42) == "root\\42");
    assert(backslash.build_attachment_key(42, "x") == "att\\42_1_x");
    app::MailObjectKeyLayout relative(Kind::relative);
    assert(relative.build_mail_body_key(42) == "mail/42");
    assert(relative.build_attachment_key(42, filename) == "attachment/42/1_" + sanitized);
    assert(relative.build_attachment_key(7, "") == "attachment/7/2_attachment");
    app::MailObjectKeyLayout s3(Kind::s3);
    assert(s3.build_mail_body_key(42) == "mail/42");
    assert(s3.build_attachment_key(42, filename) == "attachments/42/0_" + filename);
    assert(s3.build_attachment_key(7, "") == "attachments/7/1_");
    app::NullStorageProvider discard;
    assert(app::build_mail_body_key(discard, 42) == "/dev/null/42");
    assert(app::build_attachment_key(discard, 42, filename) == "/dev/null/42_" + filename);
    assert(app::build_attachment_key(discard, 7, "") == "/dev/null/7_");
    assert(app::build_attachment_key(discard, 42, filename) == "/dev/null/42_" + filename); // no sequence
    // Merely construct remote providers and query their policy. No readiness,
    // open/read/write or other HTTP operations are invoked in these tests.
#if PROTORELAY_ENABLE_HDFS_WEB_STORAGE
    app::HdfsWebStorageProvider hdfs("", "", "");
    assert(app::build_mail_body_key(hdfs, 42) == "mail/42");
    assert(app::build_attachment_key(hdfs, 42, filename) == "attachment/42/1_" + sanitized);
#endif
#ifdef ENABLE_S3_STORAGE
    app::S3StorageProvider remote("", "", "", "");
    assert(app::build_mail_body_key(remote, 42) == "mail/42");
    assert(app::build_attachment_key(remote, 42, filename) == "attachments/42/0_" + filename);
#endif
}
void legacy_stream_number_spelling() {
    struct GroupedNumbers final : std::numpunct<char> {
        char do_thousands_sep() const override { return ','; }
        std::string do_grouping() const override { return "\3"; }
    };
    const std::locale saved = std::locale();
    std::locale::global(std::locale(saved, new GroupedNumbers));
    app::MailObjectKeyLayout local(Kind::local, "/mail", "/att"), relative(Kind::relative), s3(Kind::s3);
    const auto local_key = local.build_mail_body_key(12345);
    const auto relative_key = relative.build_attachment_key(12345, "x");
    const auto s3_key = s3.build_mail_body_key(12345);
    std::locale::global(saved);
    assert(local_key == "/mail/12,345" && relative_key == "attachment/12,345/1_x" && s3_key == "mail/12345");
}
void local_and_async_preserve_policy_and_metadata() {
    const auto root = test_support::temp_path("mail-layout-local");
    auto inner = std::make_shared<app::LocalFileStorageProvider>(root + "/body", root + "/att");
    pr::IoError error;
    assert(inner->ensure_ready(error));
    assert(std::filesystem::is_directory(root + "/body") && std::filesystem::is_directory(root + "/att"));
    std::vector<std::function<void()>> tasks;
    auto first = std::make_shared<app::AsyncStorageProvider>(inner, [&](auto task) { tasks.push_back(std::move(task)); });
    auto second = std::make_shared<app::AsyncStorageProvider>(first, nullptr);
    const auto body = app::build_mail_body_key(second, 42);
    assert(body == root + "/body/42");
    assert(app::build_attachment_key(inner, 42, "x") == root + "/att/42_1_x");
    assert(app::build_attachment_key(first, 42, "x") == root + "/att/42_2_x");
    assert(app::build_attachment_key(second, 7, "x") == root + "/att/7_3_x");
    assert(tasks.empty()); // Policy evaluation never turns into queued IO.
    auto independent = std::make_shared<app::LocalFileStorageProvider>(root + "/body", root + "/att");
    assert(app::build_attachment_key(independent, 42, "x") == root + "/att/42_1_x");
    auto cache = std::dynamic_pointer_cast<pr::storage::IObjectMetadataCache>(second);
    assert(cache && cache->save_metadata(body, "mime", "application-owned"));
    std::string value;
    assert(inner->load_metadata(body, "mime", value) && value == "application-owned");
    assert(cache->load_metadata(body, "mime", value) && value == "application-owned");
    std::weak_ptr<app::LocalFileStorageProvider> lifetime = inner;
    inner.reset(); first.reset();
    assert(!lifetime.expired() && app::build_attachment_key(second, 42, "x") == root + "/att/42_4_x");
    second.reset(); cache.reset(); assert(lifetime.expired());
    // Generic local IO prepares caller-supplied directories without mail layout.
    pr::storage::LocalFileStorageProvider generic({root + "/generic", ""});
    assert(generic.ensure_ready(error) && std::filesystem::is_directory(root + "/generic"));
}
void distributed_full_relative_key_placement_and_replicas() {
    std::vector<std::string> roots;
    for (int i = 0; i < 3; ++i) roots.push_back(test_support::temp_path("mail-layout-replica-" + std::to_string(i)) + "/");
    app::DistributedFileStorageProvider provider({"", roots[0], roots[1], "", roots[2]}, 2);
    pr::IoError error; assert(provider.ensure_ready(error));
    const auto expected = [&](const std::string& relative) {
        return roots[std::hash<std::string>{}(relative) % roots.size()] + relative;
    };
    const std::string relative = "mail/42";
    const auto body = app::build_mail_body_key(provider, 42);
    assert(body == expected(relative));
    assert(app::build_attachment_key(provider, 42, filename) == expected("attachment/42/1_" + sanitized));
    assert(app::build_attachment_key(provider, 7, "") == expected("attachment/7/2_attachment"));
    assert(provider.resolve_object_key("objects/arbitrary") == expected("objects/arbitrary"));
    assert(provider.append_binary(body, "payload", 7, error));
    const auto primary = std::hash<std::string>{}(relative) % roots.size();
    for (size_t i = 0; i < roots.size(); ++i)
        assert(std::filesystem::exists(roots[i] + relative) == (i == primary || i == (primary + 1) % roots.size()));
    std::filesystem::remove(body);
    std::string data; assert(provider.read_all(body, data, error) && data == "payload"); // replica failover uses same hash
    assert(provider.remove_object(body, error));
    for (const auto& root : roots) assert(!std::filesystem::exists(root + relative));
}
class NoLayoutProvider final : public pr::storage::NullStorageProvider {
public:
    int io_calls = 0;
    bool append_binary(const std::string&, const char*, size_t, pr::IoError&) override { ++io_calls; return true; }
    bool read_all(const std::string&, std::string&, pr::IoError&) override { ++io_calls; return true; }
};
void missing_layout_is_explicit_and_never_io() {
    auto generic = std::make_shared<NoLayoutProvider>();
    auto wrapped = std::make_shared<app::AsyncStorageProvider>(generic, nullptr);
    for (const auto& provider : std::vector<std::shared_ptr<pr::storage::IStorageProvider>>{generic, wrapped, nullptr}) {
        int rejected = 0;
        try { (void)app::build_mail_body_key(provider, 42); } catch (const std::logic_error&) { ++rejected; }
        try { (void)app::build_attachment_key(provider, 42, "../local"); } catch (const std::logic_error&) { ++rejected; }
        assert(rejected == 2);
    }
    assert(!generic->io_calls);
    auto cache = std::dynamic_pointer_cast<pr::storage::IObjectMetadataCache>(wrapped);
    std::string value;
    assert(cache && !cache->save_metadata("opaque", "mime", "data") && !cache->load_metadata("opaque", "mime", value));
}
void attachment_sequences_are_thread_safe() {
    app::MailObjectKeyLayout layout(Kind::relative);
    std::vector<std::vector<std::string>> keys(4);
    std::vector<std::thread> threads;
    for (size_t i = 0; i < keys.size(); ++i) threads.emplace_back([&, i] {
        for (int j = 0; j < 40; ++j) keys[i].push_back(layout.build_attachment_key(42, "x"));
    });
    for (auto& thread : threads) thread.join();
    std::set<std::string> unique;
    for (auto& batch : keys) unique.insert(batch.begin(), batch.end());
    assert(unique.size() == 160);
    for (int i = 1; i <= 160; ++i) assert(unique.count("attachment/42/" + std::to_string(i) + "_x"));
}
void configuration_fields_and_defaults_are_preserved() {
    const auto defaults = app::StorageConfig::from_json(nlohmann::json::object());
    assert(defaults.provider == "local" && defaults.local.mail_path.empty() && defaults.local.attachment_path.empty());
    assert(defaults.s3.endpoint == "http://127.0.0.1:9000" && defaults.s3.bucket == "protorelay" &&
           defaults.s3.access_key == "minioadmin" && defaults.s3.secret_key == "minioadmin" && defaults.s3.region == "us-east-1");
    assert(defaults.s3.timeout_ms == 5000 && defaults.s3.use_path_style && defaults.s3.max_write_buffer_bytes == 64ull * 1024 * 1024);
    assert(defaults.hdfs.endpoint == "http://127.0.0.1:9870" && defaults.hdfs.base_path == "/mail-system" && defaults.hdfs.user == "hdfs");
    assert(defaults.hdfs.timeout_ms == 5000 && defaults.hdfs.replication == 1 && defaults.hdfs.max_write_buffer_bytes == 64ull * 1024 * 1024);
    assert(defaults.distributed.roots.empty() && defaults.distributed.replica_count == 1);
    const auto parsed = app::StorageConfig::from_json(nlohmann::json::parse(R"({
        "provider":"s3", "local":{"mail_path":"body","attachment_path":"/absolute/att"},
        "s3":{"endpoint":"endpoint","bucket":"bucket","access_key":"access","secret_key":"secret",
              "region":"region","timeout_ms":9,"use_path_style":false,"max_write_buffer_bytes":123},
        "hdfs":{"endpoint":"hdfs","base_path":"base","user":"user","timeout_ms":8,"replication":3,"max_write_buffer_bytes":456},
        "distributed":{"roots":["r1","","/r2"],"replica_count":2}})"), "/base");
    assert(parsed.provider == "s3" && parsed.local.mail_path == "/base/body" && parsed.local.attachment_path == "/absolute/att");
    assert(parsed.s3.endpoint == "endpoint" && parsed.s3.bucket == "bucket" && parsed.s3.access_key == "access" &&
           parsed.s3.secret_key == "secret" && parsed.s3.region == "region" && parsed.s3.timeout_ms == 9 && !parsed.s3.use_path_style && parsed.s3.max_write_buffer_bytes == 123);
    assert(parsed.hdfs.endpoint == "hdfs" && parsed.hdfs.base_path == "base" && parsed.hdfs.user == "user" &&
           parsed.hdfs.timeout_ms == 8 && parsed.hdfs.replication == 3 && parsed.hdfs.max_write_buffer_bytes == 456);
    assert((parsed.distributed.roots == std::vector<std::string>{"/base/r1", "/r2"}) && parsed.distributed.replica_count == 2);
    assert(app::provider_type_from_string("distributed") == app::StorageProviderType::Distributed);
    assert(app::provider_type_from_string("hdfs_web") == app::StorageProviderType::HdfsWeb);
    assert(app::provider_type_from_string("s3") == app::StorageProviderType::S3);
    assert(app::provider_type_from_string("null") == app::StorageProviderType::Null);
    assert(app::provider_type_from_string("unknown") == app::StorageProviderType::Local);
}
} // namespace
int main() {
    legacy_layout_vectors(); legacy_stream_number_spelling();
    local_and_async_preserve_policy_and_metadata();
    distributed_full_relative_key_placement_and_replicas(); missing_layout_is_explicit_and_never_io();
    attachment_sequences_are_thread_safe(); configuration_fields_and_defaults_are_preserved();
    std::cout << "Mail object layout/configuration contracts passed\n";
}
