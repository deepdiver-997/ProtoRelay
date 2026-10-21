#pragma once

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>

namespace test_support {
// Each process owns its directory; no test may delete another process's files.
class TestTempRoot {
public:
    TestTempRoot() {
        const char* configured = std::getenv("AGENT_TMP_ROOT");
        const auto base = configured && *configured
            ? std::filesystem::path(configured) : std::filesystem::temp_directory_path();
        std::filesystem::create_directories(base);
        std::random_device random;
        for (unsigned attempt = 0; attempt != 128; ++attempt) {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path_ = base / ("protorelay-tests-" + std::to_string(stamp) + "-" + std::to_string(random()));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) return;
            if (error) throw std::filesystem::filesystem_error("create test directory", path_, error);
        }
        throw std::runtime_error("could not allocate an isolated test directory");
    }
    ~TestTempRoot() { std::error_code error; std::filesystem::remove_all(path_, error); }
    TestTempRoot(const TestTempRoot&) = delete;
    TestTempRoot& operator=(const TestTempRoot&) = delete;
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
};

inline const std::filesystem::path& temp_root() {
    static const TestTempRoot root;
    return root.path();
}
inline std::string temp_path(const std::string& name) {
    return (temp_root() / name).string();
}
} // namespace test_support
