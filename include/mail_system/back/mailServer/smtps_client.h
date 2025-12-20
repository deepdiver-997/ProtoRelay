#pragma once

#include <queue>
#include <map>
#include <string>
#include <mutex>
#include <shared_mutex>
#include <mail_system/back/entities/mail.h>
#include "mail_system/back/db/db_pool.h"
#include "mail_system/back/db/db_service.h"
#include "mail_system/back/thread_pool/thread_pool_base.h"
#include <mail_system/back/mailServer/fsm/client/smtps_client_fsm.hpp>
#include "mail_system/back/mailServer/server_base.h"

namespace mail_system {
    class SMTPSClientSession;
    class SMTPSClientFSM;
    class SMTPSClient : public ServerBase {
        public:
            SMTPSClient(const ServerConfig& config);
            ~SMTPSClient();
            void non_ssl_accept_connection();
            // 处理新连接
            void handle_accept(std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket >>&& ssl_socket,
                const boost::system::error_code& error) override;
            void handle_non_ssl_accept(std::unique_ptr<boost::asio::ip::tcp::socket>&& socket,
                const boost::system::error_code& error);
            void post_to_queue(std::unique_ptr<mail>&& mail) {
                std::string domain = mail->to.empty() ? "" : mail->to[0].substr(mail->to[0].find('@') + 1);
                if(domain.empty()) {
                    return;
                }
                if(connection_pools_.find(domain) == connection_pools_.end()) {
                    connection_pools_[domain] = std::queue<std::unique_ptr<SMTPSClientSession>>();
                    email_mutex_[domain] = std::make_unique<std::mutex>();
                    id_mutex_[domain] = std::make_unique<std::mutex>();
                }
                email_mutex_[domain]->lock();
                if(email_queue_[domain].size() < max_mail_queue_size_) {
                    email_queue_[domain].push(std::move(mail));
                    email_mutex_[domain]->unlock();
                    return;
                }
                else {
                    email_mutex_[domain]->unlock();
                    // 邮件队列已满，处理溢出逻辑（如丢弃邮件或记录日志）
                    id_mutex_[domain]->lock();
                    if(id_queue_[domain].size() < max_id_queue_size) {
                        id_queue_[domain].push(mail->id);
                    }
                    id_mutex_[domain]->unlock();
                    return;
                }
            }
            void post_to_queue(size_t mail_id, const std::string& domain) {
                if(connection_pools_.find(domain) == connection_pools_.end()) {
                    connection_pools_[domain] = std::queue<std::unique_ptr<SMTPSClientSession>>();
                    email_mutex_[domain] = std::make_unique<std::mutex>();
                    id_mutex_[domain] = std::make_unique<std::mutex>();
                }
                id_mutex_[domain]->lock();
                if(id_queue_[domain].size() < max_id_queue_size) {
                    id_queue_[domain].push(mail_id);
                    id_mutex_[domain]->unlock();
                    return;
                }
                else {
                    id_mutex_[domain]->unlock();
                    // 邮件队列已满，处理溢出逻辑（如丢弃邮件或记录日志）
                    return;
                }
            }
        private:
            void working_cycle();
            bool is_empty() {
                bool ret = true;
                for (const auto& pair : email_queue_) {
                    ret &= pair.second.empty();
                }
                for (const auto& pair : id_queue_) {
                    ret &= pair.second.empty();
                }
                return ret;
            }
            std::atomic<bool> stop_signal_{false};
            std::map<std::string, std::queue<std::unique_ptr<mail>>> email_queue_;
            std::map<std::string, std::queue<size_t>> id_queue_;
            int max_retries_ = 3;
            int max_mail_queue_size_ = 100;
            int max_id_queue_size = 1000;
            std::map<std::string ,std::unique_ptr<std::mutex>> email_mutex_, id_mutex_, pool_queue_mutex_;
            std::mutex cycle_mutex_, eq_mtx_, idq_mtx_;
            std::shared_mutex pool_mutex_;
            std::condition_variable cycle_cv_;
            std::shared_ptr<SMTPSClientFSM> m_fsm;
            // 连接池管理
            std::map<std::string, std::queue<std::unique_ptr<SMTPSClientSession>>> connection_pools_;
            const int max_once_ = 5;
    };
SMTPSClient::SMTPSClient(const ServerConfig& config)
    : ServerBase(config) {
    m_fsm = std::make_shared<SMTPSClientFSM>(m_ioThreadPool, m_workerThreadPool, m_dbPool);
    // 启动工作线程
    std::thread([this]() { this->working_cycle(); }).detach();
    // ssl conection from outer server
    accept_connection();
    non_ssl_accept_connection();
}
void SMTPSClient::non_ssl_accept_connection() {
    // if (!allow_insecure) {
    //     return;
    // }
    auto non_ssl_acceptor = std::make_shared<boost::asio::ip::tcp::acceptor>(
        *m_ioContext,
        boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 466)
    );
    non_ssl_acceptor->listen();
    auto socket = std::make_unique<boost::asio::ip::tcp::socket>(*m_ioContext);
    non_ssl_acceptor->async_accept(*socket, [this, non_ssl_acceptor, socket = std::move(socket)](const boost::system::error_code& error) mutable {
        if (!error) {
            // 处理非SSL连接
            std::cout << "Accepted non-SSL connection from " << socket->remote_endpoint().address().to_string() << std::endl;
            // 这里可以创建一个非SSL会话对象并开始处理
        }
        // 继续接受下一个连接
        non_ssl_accept_connection();
    });
}

void SMTPSClient::handle_non_ssl_accept(std::unique_ptr<boost::asio::ip::tcp::socket>&& socket,
    const boost::system::error_code& error) {
    if (!error) {
        std::cout << "Accepted non-SSL connection from " << socket->remote_endpoint().address().to_string() << std::endl;
        // 不停的读取要转发出去的邮件 自定义协议 buffer稍后改为成员变量
        socket->async_read_some(boost::asio::buffer(new char[1024], 1024),
            [this, sock = std::move(socket)](const boost::system::error_code& ec, std::size_t bytes_transferred) mutable {
                if (!ec) {
                    // 处理读取的数据
                    int n_mail = 0, n_id = 0;
                    while (n_mail--) {
                        // 模拟实际接收的邮件
                        auto m = std::make_unique<mail>();
                        post_to_queue(std::move(m));
                    }
                    while (n_id--) {
                        size_t id = 0;
                        std::string domain;
                        post_to_queue(id, domain);
                    }
                }
            });
    }
    // 继续接受下一个连接
    non_ssl_accept_connection();
}

void SMTPSClient::working_cycle() {
    while (!stop_signal_) {
        if(is_empty()) {
            std::unique_lock<std::mutex> lock(cycle_mutex_);
            cycle_cv_.wait(lock, [this]() { return stop_signal_ || !is_empty(); });
        }
        std::queue<std::unique_ptr<mail>> mails;
        {
            std::unique_lock<std::mutex> lock(eq_mtx_);
            for (auto& pair : email_queue_)
                if (!pair.second.empty()) {
                    std::lock_guard<std::mutex> email_lock(*email_mutex_[pair.first]);
                    lock.unlock();
                    for (int i = 0; i < max_once_ && !email_queue_.empty(); ++i) {
                        mails.push(std::move(email_queue_[pair.first].front()));
                        email_queue_[pair.first].pop();
                    }
                }
        }
        if (mails.empty()) {
            std::unique_lock<std::mutex> lock(idq_mtx_);
            for (auto& pair : id_queue_)
                if (!pair.second.empty()) {
                    std::lock_guard<std::mutex> id_lock(*id_mutex_[pair.first]);
                    lock.unlock();
                    for (int i = 0; i < max_once_ && !email_queue_.empty(); ++i) {
                        int id = id_queue_[pair.first].front();
                        id_queue_[pair.first].pop();
                        // to do: 从数据库获取邮件逻辑
                        std::unique_ptr<mail> email = std::make_unique<mail>();
                        mails.push(std::move(email));
                    }
                }
        }
        {
            if (mails.empty()) {
                continue;
            }
            std::string domain = mails.front()->to.empty() ? "" : mails.front()->to[0].substr(mails.front()->to[0].find('@') + 1);
            if (domain.empty()) {}
            std::shared_lock<std::shared_mutex> read_lock(pool_mutex_);
            if (connection_pools_.find(domain) == connection_pools_.end()) {
                std::lock_guard<std::shared_mutex> write_lock(pool_mutex_);
                if (connection_pools_.find(domain) == connection_pools_.end())
                    connection_pools_[domain] = std::queue<std::unique_ptr<SMTPSClientSession>>();
            }
            std::lock_guard<std::mutex> pool_lock(*pool_queue_mutex_[domain]);
            auto& pool = connection_pools_[domain];
            std::unique_ptr<SMTPSClientSession> session;
            if (pool.empty()) {
                session = std::make_unique<SMTPSClientSession>(
                    std::make_unique<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>(
                        std::make_unique<boost::asio::ip::tcp::socket>(*m_ioContext),
                        m_sslContext
                    ),
                    this,
                    m_fsm.get()
                );
            } else {
                session = std::move(pool.front());
                pool.pop();
            }
            m_workerThreadPool->post([this, session = std::move(session), mails = std::move(mails)]() mutable {
                m_fsm->start_session(std::move(session), std::move(mails));
            });
        }
    }
}
} // namespace mail_system