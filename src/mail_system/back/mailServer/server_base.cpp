#include "mail_system/back/mailServer/server_base.h"
#include "mail_system/back/outbound/cares_dns_resolver.h"
#include "mail_system/back/entities/mail.h"
#include "mail_system/back/common/logger.h"
#include <iostream>
#include <fstream>

namespace mail_system {

ServerBase::ServerBase(const ServerConfig& config,
     std::shared_ptr<ThreadPoolBase> ioThreadPool,
      std::shared_ptr<ThreadPoolBase> wokerThreadPool,
       std::shared_ptr<DBPool> dbPool) 
    : m_ioThreadPool(ioThreadPool),
      m_workerThreadPool(wokerThreadPool),
      m_dbPool(dbPool),
      ssl_in_worker(config.ssl_in_worker),
            m_domain(config.system_domain.empty() ? std::string("example.com") : config.system_domain),
            m_config(config),
      m_ssl_endpoint(boost::asio::ip::make_address(config.address), config.ssl_port),
      m_tcp_endpoint(boost::asio::ip::make_address(config.address), config.tcp_port),
      m_sslContext(boost::asio::ssl::context::sslv23),
      m_enable_ssl(config.enable_ssl || config.use_ssl),
      m_enable_tcp(config.enable_tcp),
      has_listener_thread(false),
      m_state(ServerState::Stopped) {
try {
        // {
        //     std::fstream mail_storage_dir_check(config.mail_storage_path);
        //     if (!mail_storage_dir_check.is_open()) {
        //         // 目录不存在，尝试创建
        //         if (!std::filesystem::create_directories(config.mail_storage_path)) {
        //             throw std::runtime_error("Failed to create mail storage directory: " + config.mail_storage_path);
        //         }
        //     }
        // }
        Logger::get_instance().init(config.log_file, Logger::string_to_level(config.log_level));
        LOG_SERVER_INFO("Logger initialized with level: {}", config.log_level);

        // check mail and attachment storage path and create if not exists
        if (!std::filesystem::exists(config.mail_storage_path)) {
            if (!std::filesystem::create_directories(config.mail_storage_path)) {
                throw std::runtime_error("Failed to create mail storage directory: " + config.mail_storage_path);
            }
        }
        if (!std::filesystem::exists(config.attachment_storage_path)) {
            if (!std::filesystem::create_directories(config.attachment_storage_path)) {
                throw std::runtime_error("Failed to create attachment storage directory: " + config.attachment_storage_path);
            }
        }

        if(config.io_thread_count > 0 && m_ioThreadPool == nullptr) {
            m_ioThreadPool = std::make_shared<IOThreadPool>(config.io_thread_count);
            m_ioThreadPool->start();
            LOG_SERVER_INFO("IOThreadPools started");
        }

        if(config.worker_thread_count > 0 && m_workerThreadPool == nullptr) {
            m_workerThreadPool = std::make_shared<BoostThreadPool>(config.worker_thread_count);
            m_workerThreadPool->start();
            LOG_SERVER_INFO("WorkerThreadPools started");
        }

        if (config.use_database && m_dbPool == nullptr) {
            if (config.db_pool_config.achieve == "mysql") {
                // initialize DB pool asynchronously with timeout, to avoid blocking server startup
                // will be improved in future versions and support sync initialization with better error handling
                // auto fut = std::async(std::launch::async, [&]() {
                //     return MySQLPoolFactory::get_instance().create_pool(
                //         config.db_pool_config,
                //         std::make_shared<MySQLService>());
                // });
                // m_dbPool = fut.wait_for(std::chrono::seconds(10)) == std::future_status::ready ? fut.get() : nullptr;
                
                // 临时改为同步初始化，以便调试
                try {
                    m_dbPool = MySQLPoolFactory::get_instance().create_pool(
                        config.db_pool_config,
                        std::make_shared<MySQLService>());
                } catch (const std::exception& e) {
                    LOG_SERVER_ERROR("Failed to create MySQL pool: {}", e.what());
                    m_dbPool = nullptr;
                }

                if (m_dbPool) {
                    LOG_SERVER_INFO("Database pool initialized successfully");
                } else {
                    LOG_SERVER_ERROR("Failed to initialize database pool");
                    return;
                }
                m_persistentQueue = std::make_shared<persist_storage::PersistentQueue>(m_dbPool, m_workerThreadPool);
                m_persistentQueue->set_local_domain(m_domain);
                m_outboundInterruptFlag = std::make_shared<std::atomic<bool>>(true);
                outbound::OutboundIdentityConfig outbound_identity;
                outbound_identity.helo_domain = m_config.outbound_helo_domain;
                outbound_identity.mail_from_domain = m_config.outbound_mail_from_domain;
                outbound_identity.rewrite_header_from = m_config.outbound_rewrite_header_from;
                outbound_identity.dkim_enabled = m_config.outbound_dkim_enabled;
                outbound_identity.dkim_selector = m_config.outbound_dkim_selector;
                outbound_identity.dkim_domain = m_config.outbound_dkim_domain;
                outbound_identity.dkim_private_key_file = m_config.outbound_dkim_private_key_file;

                outbound::OutboundPollingConfig outbound_polling;
                outbound_polling.busy_sleep_ms = static_cast<int>(m_config.outbound_poll_busy_sleep_ms);
                outbound_polling.backoff_base_ms = static_cast<int>(m_config.outbound_poll_backoff_base_ms);
                outbound_polling.backoff_max_ms = static_cast<int>(m_config.outbound_poll_backoff_max_ms);
                outbound_polling.backoff_shift_cap = static_cast<std::size_t>(m_config.outbound_poll_backoff_shift_cap);

                m_outboundClient = std::make_shared<outbound::SmtpOutboundClient>(
                    m_dbPool,
                    m_ioThreadPool,
                    m_workerThreadPool,
                    std::make_shared<outbound::CaresDnsResolver>(),
                    m_outboundInterruptFlag,
                    std::move(outbound_identity),
                    outbound_polling,
                    m_domain,
                    m_config.outbound_ports,
                    static_cast<int>(m_config.outbound_max_attempts)
                );
                m_persistentQueue->set_outbound_client(m_outboundClient);
                m_outboundClient->start();
            } else {
                LOG_SERVER_ERROR("Unsupported database achieve: {}", config.db_pool_config.achieve);
                return;
            }
        }

        if(m_dbPool == nullptr) {
            LOG_SERVER_WARN("Database pool is not initialized");
        }

        // m_client_fsm = std::make_shared<ClientFSM>();

        m_ioContext = std::make_shared<boost::asio::io_context>();
        m_resolver = std::make_shared<boost::asio::ip::tcp::resolver>(*m_ioContext);
        m_workGuard = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(m_ioContext->get_executor());
        
        // 初始化SSL接受器
        if (m_enable_ssl) {
            // 配置SSL上下文
            m_sslContext.set_options(
                boost::asio::ssl::context::default_workarounds |
                boost::asio::ssl::context::no_sslv2 |
                boost::asio::ssl::context::no_sslv3 |
                boost::asio::ssl::context::single_dh_use
            );

            // 加载证书
            load_certificates(config.certFile, config.keyFile, config.dhFile);

            // 创建SSL接受器
            m_ssl_acceptor = std::make_shared<boost::asio::ip::tcp::acceptor>(*m_ioContext);
            m_ssl_acceptor->open(m_ssl_endpoint.protocol());
            m_ssl_acceptor->set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
            m_ssl_acceptor->bind(m_ssl_endpoint);
            m_ssl_acceptor->listen();

            LOG_SERVER_INFO("SSL acceptor initialized on {}:{}", config.address, config.ssl_port);
        }

        // 初始化TCP接受器
        if (m_enable_tcp) {
            // 创建TCP接受器
            m_tcp_acceptor = std::make_shared<boost::asio::ip::tcp::acceptor>(*m_ioContext);
            m_tcp_acceptor->open(m_tcp_endpoint.protocol());
            m_tcp_acceptor->set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
            m_tcp_acceptor->bind(m_tcp_endpoint);
            m_tcp_acceptor->listen();

            LOG_SERVER_INFO("TCP acceptor initialized on {}:{}", config.address, config.tcp_port);
        }
        
        // 向后兼容：设置默认接受器
        if (m_ssl_acceptor) {
            m_acceptor = m_ssl_acceptor;
        } else if (m_tcp_acceptor) {
            m_acceptor = m_tcp_acceptor;
        }

        LOG_SERVER_INFO("Server initialized with SSL: {}, TCP: {}",
                       m_enable_ssl ? "enabled" : "disabled",
                       m_enable_tcp ? "enabled" : "disabled");

        m_state = ServerState::Paused;
    }
    catch (const std::exception& e) {
        LOG_SERVER_ERROR("Error initializing server: {}", e.what());
        throw;
    }
}

ServerBase::~ServerBase() {
    stop();
    // Logger 是全局单例，不应该在这里 shutdown
    // 应该在程序最后统一关闭
}

void ServerBase::accept_ssl_connection() {

    LOG_NETWORK_DEBUG("Waiting for SSL connection...");
    // 创建新的TCP socket和SSL流
    auto socket = std::make_unique<boost::asio::ip::tcp::socket>(std::static_pointer_cast<IOThreadPool>(m_ioThreadPool)->get_io_context());
    auto ssl_socket = std::make_unique<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>(std::move(*socket), m_sslContext);
    // 获取底层socket   避免某些平台先进行lambda捕获再进行参数求值引起的段错误
    auto &lowest_socket = ssl_socket->next_layer();
    // 接受连接
    m_ssl_acceptor->async_accept(
        lowest_socket,
        [this, ssl_socket = std::move(ssl_socket)](const boost::system::error_code& ec) mutable {
            if (!ec) {
                LOG_NETWORK_INFO("New SSL connection accepted");
                LOG_NETWORK_DEBUG("Start handling SSL connection");
                // 会话已建立
                handle_accept(std::move(ssl_socket), ec);
            }
            else {
                if (m_state.load() == ServerState::Running) {
                    LOG_NETWORK_ERROR("Error accepting SSL connection: {}", ec.message());
                    return;
                }
            }

            // 继续接受连接
            if(m_state.load() == ServerState::Running)
                accept_ssl_connection();
        }
    );
}

void ServerBase::accept_tcp_connection() {
    // if (!m_enable_tcp || !m_tcp_acceptor) {
    //     return;
    // }

    LOG_NETWORK_DEBUG("Waiting for TCP connection...");
    auto socket = std::make_unique<boost::asio::ip::tcp::socket>(*m_ioContext);
    auto& peer_socket = *socket;

    m_tcp_acceptor->async_accept(peer_socket,
        [this, socket = std::move(socket)](const boost::system::error_code& error) mutable {
            if (!error) {
                LOG_NETWORK_INFO("New TCP connection accepted from {}",
                              socket->remote_endpoint().address().to_string());
                // 处理TCP连接
                handle_tcp_accept(std::move(socket), error);
            } else {
                if (m_state.load() == ServerState::Running) {
                    LOG_NETWORK_ERROR("Error accepting TCP connection: {}", error.message());
                    return;
                }
            }

            // 继续接受下一个连接
            if(m_state.load() == ServerState::Running)
            accept_tcp_connection();
        });
}

// 向后兼容接口
void ServerBase::accept_connection() {
    accept_ssl_connection();
}

void ServerBase::non_ssl_accept_connection() {
    // 向后兼容：使用新的TCP连接处理
    accept_tcp_connection();
}

void ServerBase::start() {
    if(m_state.load() == ServerState::Running) {
        return;
    }
    if (m_state.load() != ServerState::Stopped) {
        m_state.store(ServerState::Running);
    if (m_outboundInterruptFlag) {
        m_outboundInterruptFlag->store(true);
    }
    if (!m_acceptor->is_open()) {
        m_acceptor->open(m_acceptor->local_endpoint().protocol());
        m_acceptor->listen();
    }
        
            try {
            // 开始接受连接
            if(has_listener_thread == false) {
                m_listenerThread = std::thread([this]() {
                    // 异步接受连接
                    if (m_enable_ssl)
                        accept_ssl_connection();
                    if (m_enable_tcp)
                        accept_tcp_connection();
                    m_ioContext->run();
                });
                has_listener_thread = true;
                m_listenerThread.detach();
            }
            else {
                boost::asio::post(*m_ioContext, [this]() {
                    // 异步接受连接
                    if (m_enable_ssl)
                        accept_ssl_connection();
                    if (m_enable_tcp)
                        accept_tcp_connection();
                });
            }
            LOG_SERVER_INFO("Server started");
        }
        catch (const std::exception& e) {
            LOG_SERVER_ERROR("Error starting server: {}", e.what());
            stop();
        }
    }
}

void ServerBase::run() {
    // 保持主线程运行，直到服务器停止
    while (m_state.load() == ServerState::Running || m_state.load() == ServerState::Paused) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    // std::this_thread::sleep_for(std::chrono::minutes(1));
}

void ServerBase::stop(ServerState next_state) {

    if(next_state == ServerState::Running) {
        LOG_SERVER_WARN("Can't switch to running state in function ServerBase::stop");
        return;
    }

    if (m_state.load() == ServerState::Running) {
        try {
            m_state.store(next_state);
            if (m_outboundInterruptFlag) {
                m_outboundInterruptFlag->store(false);
            }
            // boost::asio::post(*m_ioContext, [this]() {
            //     m_acceptor->close();
            //     if(m_workGuard->owns_work() && m_state == ServerState::Stopped)
            //         m_workGuard->reset();
            // });
            if(m_listenerThread.joinable())
                m_listenerThread.join();
            LOG_SERVER_INFO("Listener thread stopped");
            
            // 先关闭 PersistentQueue，停止其 worker 线程
            if (m_outboundClient) {
                m_outboundClient->stop();
                LOG_SERVER_INFO("Outbound client stopped");
            }

            if(m_persistentQueue) {
                m_persistentQueue->shutdown();
                LOG_SERVER_INFO("PersistentQueue shutdown");
            }
            
            if(m_ioThreadPool)
                m_ioThreadPool->stop();
            if(m_workerThreadPool)
                m_workerThreadPool->stop();
            LOG_SERVER_INFO("ThreadPools stopped");

            // 关闭接受器
            boost::system::error_code ec;
            if (m_ssl_acceptor) {
                m_ssl_acceptor->close(ec);
                if (ec) {
                    LOG_SERVER_ERROR("Error closing SSL acceptor: {}", ec.message());
                }
            }
            if (m_tcp_acceptor) {
                m_tcp_acceptor->close(ec);
                if (ec) {
                    LOG_SERVER_ERROR("Error closing TCP acceptor: {}", ec.message());
                }
            }
            if (m_acceptor && m_acceptor != m_ssl_acceptor && m_acceptor != m_tcp_acceptor) {
                m_acceptor->close(ec);
                if (ec) {
                    LOG_SERVER_ERROR("Error closing acceptor: {}", ec.message());
                }
            }

            LOG_SERVER_INFO("Server stopped");
        }
        catch (const std::exception& e) {
            LOG_SERVER_ERROR("Error stopping server: {}", e.what());
        }
    }
}

void ServerBase::start_forward_email(mail* email) {
    if (!email) {
        LOG_SERVER_ERROR("Error: null mail pointer in start_forward_email");
        return;
    }
    
    // 创建临时shared_ptr，但注意这里email的生命周期由调用者管理
    // 这是一个临时解决方案，更好的方法是重构整个架构
    auto email_shared = std::shared_ptr<mail>(email, [](mail*) {
        // 空删除器，因为email的生命周期由外部管理
    });
    
    start_forward_email(email_shared);
}

void ServerBase::start_forward_email(std::shared_ptr<mail> email) {
    // std::sort(email->to.begin(), email->to.end(), [](const std::string& a, const std::string& b) {
    //     auto pos_a = a.find('@');
    //     auto pos_b = b.find('@');
    //     if (pos_a != std::string::npos && pos_b != std::string::npos) {
    //         return a.substr(pos_a + 1) < b.substr(pos_b + 1);
    //     }
    // });
    std::map<std::string, std::vector<std::string>> remote_domains;
    for (const auto& recipient : email->to) {
        auto pos = recipient.find('@');
        if (pos != std::string::npos && pos + 1 < recipient.size() && recipient.substr(pos + 1) != m_domain) {
            remote_domains[recipient.substr(pos + 1)].push_back(recipient);
        }
    }
    if (remote_domains.empty()) {
        return;
    }
    for (const auto& [domain, recipients] : remote_domains) {
        // 这里可以添加实际的邮件转发代码
        boost::asio::ip::tcp::endpoint edp;
        if (m_known_domains.find(domain) == m_known_domains.end()) {
            edp = *m_resolver->resolve(domain, "smtp").begin();
        } else {
            edp = boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address(m_known_domains[domain]), 465);
        }
        // 创建新的TCP socket和SSL流
        auto socket = std::make_unique<boost::asio::ip::tcp::socket>(std::static_pointer_cast<IOThreadPool>(m_ioThreadPool)->get_io_context());
        auto ssl_socket = std::make_unique<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>(std::move(*socket), m_sslContext);
        ssl_socket->lowest_layer().async_connect(edp, [this, ssl_socket = std::move(ssl_socket), recipients, edp, email](const boost::system::error_code& error) mutable {
            if (!error) {
                // 连接成功，进行邮件转发
                // m_client_fsm->start(std::dynamic_pointer_cast<mail_system::SessionBase>(std::make_shared<ClientSession>(email, std::move(ssl_socket), this, m_client_fsm, recipients)));
            } else {
                LOG_SERVER_ERROR("Error connecting to {}: {}", edp.address().to_string(), error.message());
            }
        });
    }
}

void ServerBase::load_known_domains(const char* domain_file) {
    // 这里可以添加加载已知域名的代码
}

ServerState ServerBase::get_state() const {
    return m_state.load();
}

std::shared_ptr<boost::asio::io_context> ServerBase::get_io_context() {
    return m_ioContext;
}

boost::asio::ssl::context& ServerBase::get_ssl_context() {
    return m_sslContext;
}

std::shared_ptr<boost::asio::ip::tcp::acceptor> ServerBase::get_ssl_acceptor() {
    return m_ssl_acceptor;
}

std::shared_ptr<boost::asio::ip::tcp::acceptor> ServerBase::get_tcp_acceptor() {
    return m_tcp_acceptor;
}

std::shared_ptr<boost::asio::ip::tcp::acceptor> ServerBase::get_acceptor() {
    return m_acceptor;  // 向后兼容
}

void ServerBase::load_certificates(const std::string& cert_file, const std::string& key_file, const std::string& dh_file) {
    try {
        // 检查证书文件是否存在
        if (!std::ifstream(cert_file.c_str()).good()) {
            throw std::runtime_error("Certificate file not found: " + cert_file);
        }
        if (!std::ifstream(key_file.c_str()).good()) {
            throw std::runtime_error("Private key file not found: " + key_file);
        }

        // 加载证书
        m_sslContext.use_certificate_chain_file(cert_file);
        
        // 加载私钥
        m_sslContext.use_private_key_file(key_file, boost::asio::ssl::context::pem);
        
        // 验证私钥
        if(!dh_file.empty()) {
            try {
                if (!std::ifstream(dh_file.c_str()).good()) {
                    LOG_SERVER_WARN("DH file not found: {}", dh_file);
                } else {
                    m_sslContext.use_tmp_dh_file(dh_file);
                }
            } catch (const std::exception& e) {
                LOG_SERVER_WARN("Failed to load DH file: {}", e.what());
            }
        }

        LOG_SERVER_INFO("SSL certificates loaded successfully");
    }
    catch (const std::exception& e) {
        LOG_SERVER_ERROR("Error loading SSL certificates: {}", e.what());
        throw;
    }
}

} // namespace mail_system