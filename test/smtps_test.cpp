#include "mail_system/back/mailServer/smtps_server.h"
#include "mail_system/back/common/logger.h"
#include <iostream>
#include <memory>
#include <signal.h>
#if ENABLE_DATABASE_QUERY_DEBUG_LOG
#define ENABLE_DATABASE_QUERY_DEBUG_LOG 0
#endif
#if ENABLE_SMTP_DETAIL_DEBUG_LOG
#undef ENABLE_SMTP_DETAIL_DEBUG_LOG
#define ENABLE_SMTP_DETAIL_DEBUG_LOG 1
#endif


using namespace mail_system;

// 全局服务器指针，用于信号处理
std::unique_ptr<SmtpsServer> g_server = nullptr;

// 信号处理函数
void signal_handler(int signal) {
    LOG_SERVER_INFO("\nReceived signal {}, shutting down...", signal);
    if (g_server) {
        g_server->stop();
    }
}

int main(int argc, char* argv[]) {
    // 初始化日志系统
    Logger::get_instance().init("logs/mail_system.log", 1024 * 1024 * 5, 3, spdlog::level::debug);

    // 注册信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    LOG_SERVER_INFO("SMTPS Server V6 Starting...");

    try {
        // 加载配置
        ServerConfig config;
        if (!config.loadFromFile("config/smtpsConfig.json")) {
            std::cerr << "Failed to load config file: config/smtpsConfig.json" << std::endl;
            return 1;
        }
        std::cout << "config:\n";
        config.show();
        std::cout << std::endl;

        // 创建服务器
        g_server = std::make_unique<SmtpsServer>(config);

        // 启动服务器
        g_server->start();

        LOG_SERVER_INFO("SMTPS Server is running");
        LOG_SERVER_INFO("Press Ctrl+C to stop");

        // 主循环
        g_server->run();

        LOG_SERVER_INFO("SMTPS Server stopped");

        // 关闭日志系统
        Logger::get_instance().shutdown();

    } catch (const std::exception& e) {
        LOG_SERVER_ERROR("Error: {}", e.what());
        return 1;
    }

    return 0;
}
