// h2_multistream_test —— HTTP/2 整机多流 e2e（补 docs/local/TODO.md 第 9 项）。
//
// 此前 H2 只有纯函数层单测（h2_web_test），整机多流回归一直是手动
// curl/python。本测试在进程内起真实 H2Server（TcpServerBase 工厂、
// TCP listener、真实 IO/Worker 线程），用系统 curl --http2-prior-knowledge
// 以 h2c 先验知识直连，验证：
//   1. 单流 GET：响应确为 HTTP/2（%{http_version} == 2），内容一致；
//   2. 单流 8MiB 大文件（流控分片 DATA 路径）：字节级一致；
//   3. 单连接多流并发：--parallel 6×8MiB + 2×小文件交织，全部字节级一致
//      （流控窗口在 48MiB 汇聚流量下必然触发 WINDOW_UPDATE 交织）；
//   4. 404 路径的 H2 响应形状（:status 404）。
// 环境无 HTTP2 curl（`curl --version` 无 HTTP2 特性串）→ 退出 77（ctest skip，
// 属性在 cmake/ctest.cmake 特殊覆盖区）。
//
// 只连 127.0.0.1（AGENTS 红线）；端口由内核分配（bind 0 后重用，竞态窗口
// 极小且重试一次）。服务随进程收尾，不落任何常驻服务。
#undef NDEBUG
#include "test_temp_dir.h"
#include "framework/server_config.h"
#include "framework/thread_pool/io_thread_pool.h"
#include "framework/thread_pool/worker_thread_pool.h"
#include "framework/http/h2/h2_server.h"
#include "framework/log/log_macros.h"
#include <boost/asio.hpp>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr size_t kBigBytes = 8ull * 1024 * 1024;

const std::string kRoot = test_support::temp_path("h2_multistream_root");

char big_pattern(size_t i) { return static_cast<char>(i % 251); }

void make_fixture() {
    std::filesystem::remove_all(kRoot);
    std::filesystem::create_directories(kRoot);
    { std::ofstream f(kRoot + "/hello.txt"); f << "hello h2 multistream"; }
    std::ofstream big(kRoot + "/big.bin", std::ios::binary);
    for (size_t i = 0; i < kBigBytes; ++i) big.put(big_pattern(i));
    assert(big.good());
}

bool files_equal(const std::string& a_path, bool big) {
    std::ifstream a(a_path, std::ios::binary);
    if (!a.good()) return false;
    if (big) {
        for (size_t i = 0; i < kBigBytes; ++i) {
            char c = 0;
            if (!a.get(c) || c != big_pattern(i)) return false;
        }
        return a.peek() == EOF;
    }
    std::string body((std::istreambuf_iterator<char>(a)), std::istreambuf_iterator<char>());
    return body == "hello h2 multistream";
}

// 返回 curl 退出码；out 追加写（用于 %{http_version} 捕获）。
int run_curl(const std::string& args) {
    const std::string cmd = "curl " + args + " 2>/dev/null";
    const int rc = std::system(cmd.c_str());
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

uint16_t pick_free_port() {
    boost::asio::io_context ctx;
    boost::asio::ip::tcp::acceptor acc(ctx, {boost::asio::ip::tcp::v4(), 0});
    return acc.local_endpoint().port();
}

std::string slurp(const std::string& path) {
    std::ifstream f(path);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return s;
}

} // namespace

int main() {
    {
        FILE* v = popen("curl --version 2>/dev/null", "r");
        assert(v);
        std::string all;
        char buf[4096] = {};
        size_t n = 0;
        while ((n = fread(buf, 1, sizeof(buf), v)) > 0) all.append(buf, n);
        pclose(v);
        // HTTP2 特性串在 Features 行（macOS curl 的版本行只列库依赖）
        if (all.find("HTTP2") == std::string::npos) {
            std::cerr << "skip: curl has no HTTP2 support\n";
            return 77;
        }
    }

    pr::Logger::get_instance().set_handler({});
    make_fixture();

    const std::string tmp = test_support::temp_path("h2_multistream_out");
    std::filesystem::create_directories(tmp);

    // bring-up：bind(0) 取端口到真正 listen 有极小竞态窗口，start 失败换端口重试
    auto io = std::make_shared<pr::IOThreadPool>(2);
    io->start();
    auto wk = std::make_shared<pr::WorkerThreadPool>(2);
    wk->start();
    std::unique_ptr<pr::http::h2::H2Server> server;
    std::string base;
    for (int attempt = 0; attempt < 3 && !server; ++attempt) {
        const uint16_t port = pick_free_port();
        base = "http://127.0.0.1:" + std::to_string(port);
        pr::ServerConfig config;
        config.address = "127.0.0.1";
        config.maxConnections = 64;
        config.listeners.emplace_back(pr::ListenerType::TCP, port);
        try {
            auto candidate = std::make_unique<pr::http::h2::H2Server>(config, kRoot, io, wk);
            candidate->start();
            server = std::move(candidate);
        } catch (const std::exception&) {
            continue;   // 端口被抢：换一个重来
        }
    }
    if (!server) { std::cerr << "FAIL: could not bind any port\n"; return 1; }

    // 就绪探测：接受前 curl 会拒连，轮询至成功（有界 5s）。
    bool ready = false;
    for (int i = 0; i < 50 && !ready; ++i) {
        const std::string out = tmp + "/probe.txt";
        ready = run_curl("-s --http2-prior-knowledge --max-time 2 -o " + out +
                         " " + base + "/hello.txt") == 0 &&
                files_equal(out, false);
        if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!ready) { std::cerr << "FAIL: server never became ready\n"; return 1; }

    int failures = 0;
    auto expect = [&](bool cond, const char* name) {
        if (cond) return;
        ++failures;
        std::cerr << "FAIL: " << name << "\n";
    };

    // 1. 单流 + HTTP/2 版本断言
    {
        const std::string ver = tmp + "/ver.txt";
        expect(run_curl("-s --http2-prior-knowledge --max-time 60 -w '%{http_version}' -o " +
                        tmp + "/small.bin " + base + "/hello.txt > " + ver) == 0, "small rc");
        expect(slurp(ver) == "2", "http version is 2");
        expect(files_equal(tmp + "/small.bin", false), "small content");
    }

    // 2. 单流 8MiB（流控分片）
    expect(run_curl("-s --http2-prior-knowledge --max-time 120 -o " + tmp + "/big1.bin " +
                    base + "/big.bin") == 0, "big rc");
    expect(files_equal(tmp + "/big1.bin", true), "big content");

    // 3. 单连接多流并发：6×8MiB + 2×small 交织
    {
        std::string args = "-s --http2-prior-knowledge --parallel --parallel-immediate "
                           "--parallel-max 6 --max-time 180";
        for (int i = 1; i <= 6; ++i)
            args += " -o " + tmp + "/m" + std::to_string(i) + ".bin " + base + "/big.bin";
        for (int i = 1; i <= 2; ++i)
            args += " -o " + tmp + "/s" + std::to_string(i) + ".txt " + base + "/hello.txt";
        expect(run_curl(args) == 0, "multistream rc");
        for (int i = 1; i <= 6; ++i)
            expect(files_equal(tmp + "/m" + std::to_string(i) + ".bin", true),
                   ("multistream big " + std::to_string(i)).c_str());
        for (int i = 1; i <= 2; ++i)
            expect(files_equal(tmp + "/s" + std::to_string(i) + ".txt", false),
                   ("multistream small " + std::to_string(i)).c_str());
    }

    // 4. 404 形状
    {
        const std::string code = tmp + "/code.txt";
        expect(run_curl("-s --http2-prior-knowledge --max-time 30 -w '%{http_code}' -o /dev/null " +
                        base + "/nope.bin > " + code) == 0, "404 rc");
        expect(slurp(code) == "404", "404 status");
    }

    server->request_stop();
    server.reset();
    wk->stop();
    io->stop();
    std::filesystem::remove_all(tmp);

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "h2_multistream_test: all checks passed\n";
    return 0;
}
