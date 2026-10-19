// ──────────────────────────────────────────────────────────────────
// webmail_main — webmail API 服务器入口（第四协议前端，kickoff §三）
//
// 用法: webmailServer [config_json] [port] [doc_root]
//       默认 config=config/webmailConfig.json（文件缺失 → 程序化最小配置，
//       DB 走 NullDBPool：health/me 可用，login 一律 503 fail-closed）
//       默认 port=8081  doc_root=./build-webmail
// 装配同 mail_server_combined：bootstrap::assemble 按配置建 DB/存储/路由。
// ──────────────────────────────────────────────────────────────────
#include "framework/thread_pool/io_thread_pool.h"
#include "framework/thread_pool/worker_thread_pool.h"
#include "mail_system/back/bootstrap.h"
#include "mail_system/back/webmail/webmail_server.h"
#include "mail_system/back/common/logger.h"

#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <chrono>
#include <nlohmann/json.hpp>

using namespace mail_system;

static sigset_t g_sigset;
static std::unique_ptr<webmail::WebmailServer> g_server;

int main(int argc, char* argv[]) {
    if (argc > 1 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
        std::cout << "Usage: webmailServer [config_json] [port] [doc_root]\n"
                     "Defaults: config/webmailConfig.json, 8081, ./build-webmail\n"
                     "Config: webmail_node_id (0..1023, default 1); use a distinct Snowflake node ID per writer.\nShared DB sessions and direct durable submission require use_database=true.\n";
        return 0;
    }
    std::string config_path = argc > 1 ? argv[1] : "config/webmailConfig.json";
    int port = argc > 2 ? std::atoi(argv[2]) : 8081;
    std::string doc_root = argc > 3 ? argv[3] : "./build-webmail";

    sigemptyset(&g_sigset);
    sigaddset(&g_sigset, SIGINT);
    sigaddset(&g_sigset, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &g_sigset, nullptr);

    try {
        ServerConfig config;
        uint16_t submission_node_id = 1;
        if (std::filesystem::exists(config_path) && config.loadFromFile(config_path)) {
            LOG_SERVER_INFO("Loaded webmail config: {}", config_path);
            // webmail 专属装配配置留在入口，不扩散进 framework/ServerConfig。
            std::ifstream input(config_path);
            auto web_config = nlohmann::json::parse(input);
            if (web_config.contains("webmail_node_id")) {
                const auto& value = web_config["webmail_node_id"];
                if (!value.is_number_integer() || value < 0 || value > 1023)
                    throw std::invalid_argument("webmail_node_id must be an integer in 0..1023");
                submission_node_id = value.get<uint16_t>();
            }
        } else {
            LOG_SERVER_WARN("Config {} not loadable — minimal in-memory config (DB off)",
                            config_path);
            config.address = "0.0.0.0";
            config.maxConnections = 10000;
            config.use_database = false;
            // pr::ServerBase 初始化总会装配本地存储，给非空路径避免 create_directories("")
            config.storage.provider = "local";
            config.storage.local.mail_path = "config/storage/mail";
            config.storage.local.attachment_path = "config/storage/attachments";
        }
        // webmail 监听端口：命令行显式给 > 配置 listeners > 默认 8081
        if (port > 0 || config.listeners.empty()) {
            if (port <= 0) port = 8081;
            config.listeners.clear();
            config.listeners.emplace_back(pr::ListenerType::TCP, static_cast<uint16_t>(port));
        }

        std::cout << "Starting webmail API server on :" << port
                  << "  doc_root=" << doc_root << "\n";

        auto io_pool = std::make_shared<pr::IOThreadPool>(config.io_thread_count);
        io_pool->start();
        auto wk_pool = std::make_shared<pr::WorkerThreadPool>(config.worker_thread_count);
        wk_pool->start();
        bootstrap::Options pools;
        pools.io_pool = io_pool;
        pools.worker_pool = wk_pool;
        auto res = bootstrap::assemble(config, pools);

        g_server = std::make_unique<webmail::WebmailServer>(
            config, doc_root, res.io_pool, res.worker_pool, res.shard_router,
            nullptr, nullptr, nullptr, submission_node_id);
        g_server->start();

        LOG_SERVER_INFO("webmail API ready. try: curl http://localhost:{}/api/health", port);

        int sig = 0;
        while (g_server) {
            sigwait(&g_sigset, &sig);
            break;   // SIGINT / SIGTERM
        }

        LOG_SERVER_INFO("Shutting down webmail server...");
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
