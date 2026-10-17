// ──────────────────────────────────────────────────────────────────
// h2web_main — HTTP/2 多流会话骨架演示入口
//
// 用法:  h2web_main [port]   默认 8081
// 验证:  nghttp http://127.0.0.1:8081/        （容器内 nghttp2-client）
//        或  curl --http2-prior-knowledge http://127.0.0.1:8081/
// 说明:  HPACK 解码未接 → 不读 :path，所有请求固定回 200；
//        本 demo 意在证明"一连接多流复用"的会话结构，非完整 H2 服务器。
// ──────────────────────────────────────────────────────────────────
#include "framework/thread_pool/io_thread_pool.h"
#include "framework/thread_pool/boost_thread_pool.h"
#include "framework/db/db_pool.h"
#include "mail_system/back/router/static_shard_router.h"
#include "mail_system/back/storage/i_storage_provider.h"

// 纯 DI（2026-10-18）：pr::ServerBase 三件套缺一不可。web 静态服务器不用邮件业务
// 资源，显式传 null-object（空 router），不悄悄自建。
static std::shared_ptr<mail_system::router::IShardRouter> make_null_router() {
    return std::make_shared<mail_system::router::StaticShardRouter>(
        std::vector<std::pair<std::string, int>>{}, 0,
        std::vector<std::shared_ptr<pr::DBPool>>{},
        std::vector<std::shared_ptr<mail_system::storage::IStorageProvider>>{});
}

#include "framework/http/h2/h2_server.h"
#include "mail_system/back/common/logger.h"
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <chrono>

using namespace mail_system;

static sigset_t g_sigset;
static std::unique_ptr<pr::http::h2::H2Server> g_server;

int main(int argc, char* argv[]) {
    int port = argc > 1 ? std::atoi(argv[1]) : 8081;
    std::string doc_root = argc > 2 ? argv[2] : "./config/web";

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
        config.storage.provider = "local";
        config.storage.local.mail_path = "config/h2storage/mail";
        config.storage.local.attachment_path = "config/h2storage/attachments";

        auto io_pool = std::make_shared<pr::IOThreadPool>(config.io_thread_count);
        io_pool->start();
        auto wk_pool = std::make_shared<pr::BoostThreadPool>(config.worker_thread_count);
        wk_pool->start();
        g_server = std::make_unique<pr::http::h2::H2Server>(config, doc_root, io_pool, wk_pool, make_null_router());
        g_server->start();

        std::cout << "HTTP/2 static file server on :" << port << "  doc_root=" << doc_root
                  << "\n  test: nghttp http://127.0.0.1:" << port << "/\n"
                  << "  or:   curl --http2-prior-knowledge http://127.0.0.1:" << port << "/ -i\n";

        int sig = 0;
        while (g_server) { sigwait(&g_sigset, &sig); break; }
        g_server->request_stop();
        g_server.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        pr::Logger::get_instance().shutdown();
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << "\n";
        return 1;
    }
    return 0;
}