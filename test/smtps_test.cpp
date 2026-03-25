#include "mail_system/back/mailServer/smtps_server.h"
#include "mail_system/back/cli/help_text.h"
#include "mail_system/back/common/logger.h"
#include <iostream>
#include <memory>
#include <signal.h>
#include <string>
#include <thread>
#include <chrono>
#if ENABLE_DATABASE_QUERY_DEBUG_LOG
#define ENABLE_DATABASE_QUERY_DEBUG_LOG 0
#endif
#if ENABLE_SMTP_DETAIL_DEBUG_LOG
#undef ENABLE_SMTP_DETAIL_DEBUG_LOG
#define ENABLE_SMTP_DETAIL_DEBUG_LOG 1
#endif


using namespace mail_system;

namespace {
constexpr const char* kDefaultConfigPath = "config/smtpsConfig.json";

struct CliOptions {
    std::string config_path = kDefaultConfigPath;
    bool show_help = false;
    bool show_version = false;
};

bool parse_cli_options(int argc, char* argv[], CliOptions& options, std::string& error) {
    bool positional_config_set = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg.empty()) {
            continue;
        }

        if (arg == "-h" || arg == "--help") {
            options.show_help = true;
            return true;
        }

        if (arg == "-V" || arg == "--version") {
            options.show_version = true;
            return true;
        }

        if (arg == "-c" || arg == "--config") {
            if (i + 1 >= argc || argv[i + 1] == nullptr) {
                error = "Missing config path after " + arg;
                return false;
            }
            options.config_path = argv[++i];
            positional_config_set = true;
            continue;
        }

        if (!arg.empty() && arg[0] == '-') {
            error = "Unknown option: " + arg;
            return false;
        }

        if (positional_config_set) {
            error = "Only one config path is allowed";
            return false;
        }

        // Backward-compatible: first positional arg is config path.
        options.config_path = arg;
        positional_config_set = true;
    }

    return true;
}
}

// 全局服务器指针，用于信号处理
std::unique_ptr<SmtpsServer> g_server = nullptr;

// 信号处理函数
void signal_handler(int signal) {
    LOG_SERVER_INFO("\nReceived signal {}, shutting down...", signal);
    if (g_server) {
        g_server->stop();
        g_server.reset();
    }
}

int main(int argc, char* argv[]) {
    CliOptions options;
    std::string parse_error;
    if (!parse_cli_options(argc, argv, options, parse_error)) {
        std::cerr << parse_error << "\n\n";
        std::cerr << cli::render_help_text(argv[0]);
        return 2;
    }

    if (options.show_help) {
        std::cout << cli::render_help_text(argv[0]);
        return 0;
    }

    if (options.show_version) {
        std::cout << cli::render_version_text();
        return 0;
    }

    // 注册信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    #ifdef GNU_LINUX
    std::cout << "Remember to execute 'sudo setcap 'cap_net_bind_service=+ep' " << argv[0] << "' to allow binding to privileged ports without running as root." << std::endl;
    #endif

    std::cout << "ProtoRelay starting..." << std::endl;

    try {
        std::string config_path = options.config_path;

        // 加载配置
        ServerConfig config;
        if (!config.loadFromFile(config_path)) {
            std::cerr << "Failed to load config file: " << config_path << std::endl;
            return 1;
        }

        // Initialize logger from config so runtime log path/level are respected.
        Logger::get_instance().init(
            config.log_file,
            1024 * 1024 * 5,
            3,
            Logger::string_to_level(config.log_level),
            config.log_to_console,
            config.log_to_file
        );
        LOG_SERVER_INFO("Loaded config file: {}", config_path);
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

        // 【关键】手动释放服务器资源，确保在 Logger shutdown 之前完成所有清理
        g_server.reset();  // 触发 ServerBase 析构 → PersistentQueue 析构 → worker 线程退出
        
        // 等待所有后台线程完全退出（PersistentQueue worker、worker_pool 任务等）
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // 现在可以安全关闭日志系统了
        Logger::get_instance().shutdown();

    } catch (const std::exception& e) {
        LOG_SERVER_ERROR("Error: {}", e.what());
        std::cerr << "Exception caught in main: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        LOG_SERVER_ERROR("Unknown exception caught in main");
        std::cerr << "Unknown exception in main()" << std::endl;
        return 1;
    }

    return 0;
}
