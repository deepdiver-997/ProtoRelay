// Logging backend boundary: custom handlers, no-spdlog formatting, filtering,
// concurrent delivery, rotation, and compressed-log compatibility.
#undef NDEBUG
#include "framework/log/log_macros.h"
#include "framework/log/log_backends.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

namespace {
std::string read(const std::filesystem::path& path) {
    std::ifstream file(path);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void verify_sink(std::shared_ptr<pr::LogSink> sink, const std::filesystem::path& path) {
    sink->write({pr::LogLevel::warn, "CUSTOM", "hello {world}", {"source.cpp", 42, "test"}});
    sink->flush();
    const auto text = read(path);
    assert(text.find("[CUSTOM]") != std::string::npos);
    assert(text.find("source.cpp:42") != std::string::npos);
    assert(text.find("hello {world}") != std::string::npos);
}
}

int main() {
    using pr::LogLevel;
    auto& logger = pr::Logger::get_instance();
    assert(pr::format_log("{} {{literal}} {} {}", 17, true, static_cast<unsigned char>(65)) == "17 {literal} true 65");
    assert(pr::format_log("{}", static_cast<const char*>(nullptr)) == "(null)");
    for (const auto* text : {"{", "}", "{:x}", "{} {}"}) {
        bool rejected = false;
        try { (void)pr::format_log(text, 1); } catch (const std::invalid_argument&) { rejected = true; }
        assert(rejected);
    }
    assert(pr::Logger::string_to_level("error") == LogLevel::err);

    std::vector<pr::LogRecord> records;
    int flushes = 0;
    logger.set_handler([&](const pr::LogRecord& record) { records.push_back(record); }, [&] { ++flushes; });
    logger.set_level(LogLevel::info);
    // Application initialization must never replace a user-supplied handler.
    logger.init("", 0, 0, LogLevel::off, false, true);
    LOG_AT("MY_MODULE", info, "value={}", 123);
    assert(records.size() == 1 && records[0].message == "value=123");
    assert(records[0].module == "MY_MODULE" && records[0].source.line > 0);
    assert(std::string(records[0].source.function) == "main");
    LOG_AT("MY_MODULE", debug, "filtered");
    logger.set_module_level("FUTURE_MODULE", LogLevel::err);
    LOG_AT("FUTURE_MODULE", warn, "filtered even before first use");
    LOG_AT("FUTURE_MODULE", err, "kept");
    logger.set_level(LogLevel::trace);
    LOG_AT("FUTURE_MODULE", info, "module override survives global update");
    assert(records.size() == 2);
    logger.flush();
    assert(flushes == 1);

    const auto errors = logger.errors_total();
    LOG_AT("FORMAT", info, "{:unsupported}", 1);
    logger.set_handler([](const pr::LogRecord&) { throw std::runtime_error("handler failed"); },
                       [] { throw std::runtime_error("flush failed"); });
    LOG_AT("THROW", info, "safe");
    logger.flush();
    assert(logger.errors_total() == errors + 3);

    // Callback can replace itself: the registry lock must not cover invocation.
    logger.set_handler([&](const pr::LogRecord&) { logger.set_handler({}); });
    LOG_AT("REENTRANT", info, "replace");
    assert(!logger.should_log("REENTRANT", LogLevel::info));

    std::atomic<int> delivered{0};
    logger.set_handler([&](const pr::LogRecord&) { ++delivered; });
    std::vector<std::thread> writers;
    for (int i = 0; i < 4; ++i) writers.emplace_back([] {
        for (int j = 0; j < 100; ++j) LOG_AT("CONCURRENT", info, "{}", j);
    });
    for (auto& writer : writers) writer.join();
    assert(delivered == 400);

    auto dir = std::filesystem::temp_directory_path() /
        ("protorelay_logger_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    pr::LogConfig config{(dir / "standard.log").string(), 0, 2, false, true};
    verify_sink(pr::make_standard_log_sink(config), config.file);
#if PROTORELAY_HAS_SPDLOG
    config.file = (dir / "spdlog.log").string();
    verify_sink(pr::make_spdlog_log_sink(config), config.file);
#endif
    config.file = (dir / "rotate.log").string();
    config.max_file_size = 160;
    auto sink = pr::make_standard_log_sink(config);
    for (int i = 0; i < 10; ++i)
        sink->write({LogLevel::warn, "ROTATE", "sequence=" + std::to_string(i), {}});
    sink->flush();
    assert(read(config.file).find("sequence=9") != std::string::npos);
    assert(std::filesystem::exists(dir / "rotate.1.log"));
    assert(std::filesystem::exists(dir / "rotate.2.log"));
    assert(!std::filesystem::exists(dir / "rotate.3.log"));
    sink.reset();

    // The log-transform tool requires this exact hash|arguments wire format.
    pr::log_pure_init((dir / "pure.log").string());
    pr::log_pure_write(0xabc, 12, "text", true);
    pr::log_pure_write(0xdef); // Different template instantiation, same output.
    pr::log_pure_flush();
    assert(read(dir / "pure.log") == "0x0000000000000abc|12|text|true\n0x0000000000000def\n");
    pr::log_pure_init((dir / "other.log").string()); // Close file before cleanup.
    logger.shutdown();
    std::filesystem::remove_all(dir);
}
