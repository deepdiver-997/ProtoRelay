// ──────────────────────────────────────────────────────────────────
// web_server_main — HTTP/1.1 静态文件服务器入口（framework/http 的演示应用）
//
// 用法:  web_server_main [doc_root] [port]
//       默认  doc_root=./config/web  port=8080
// 对照 smtps_test 的 main：少配置加载，直接程序化填 ServerConfig。
// ──────────────────────────────────────────────────────────────────
#include "framework/thread_pool/io_thread_pool.h"
#include "framework/thread_pool/worker_thread_pool.h"
#include "framework/http/http_server.h"
#include "framework/log/log_macros.h"
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <chrono>

using namespace pr;

static sigset_t g_sigset;
static std::unique_ptr<pr::http::HttpServer> g_server;

int main(int argc, char* argv[]) {
    std::string doc_root = argc > 1 ? argv[1] : "./config/web";
    int port = argc > 2 ? std::atoi(argv[2]) : 8080;

    // 信号处理：阻塞 → sigwait 接管（同 smtps_test）
    sigemptyset(&g_sigset);
    sigaddset(&g_sigset, SIGINT);
    sigaddset(&g_sigset, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &g_sigset, nullptr);

    pr::Logger::get_instance().init("", 1024 * 1024 * 5, 3,
                                    pr::Logger::string_to_level("info"),
                                    /*console=*/true, /*file=*/false);

    try {
        ServerConfig config;
        config.address = "0.0.0.0";
        config.maxConnections = 10000;
        config.listeners.emplace_back(pr::ListenerType::TCP, static_cast<uint16_t>(port));

        std::cout << "Starting HTTP/1.1 static server on :" << port
                  << "  doc_root=" << doc_root << "\n";

        auto io_pool = std::make_shared<pr::IOThreadPool>(config.io_thread_count);
        io_pool->start();
        auto wk_pool = std::make_shared<pr::WorkerThreadPool>(config.worker_thread_count);
        wk_pool->start();
        g_server = std::make_unique<pr::http::HttpServer>(config, doc_root, io_pool, wk_pool);
        g_server->start();

        LOG_SERVER_INFO("HTTP server running. Ctrl+C to stop.");
        std::cout << "  try: curl -v http://localhost:" << port << "/index.html\n";

        int sig = 0;
        while (g_server) {
            sigwait(&g_sigset, &sig);
            break;   // SIGINT / SIGTERM
        }

        LOG_SERVER_INFO("Shutting down...");
        g_server->request_stop();
        g_server.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        pr::Logger::get_instance().shutdown();
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << "\n";
        return 1;
    }
    return 0;
}