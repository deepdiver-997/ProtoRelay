#include "framework/net/asio_server_transport.h"
#include "framework/net/asio_connection.h"
#include "framework/net/asio_error.h"
#include "framework/log/log_macros.h"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <atomic>
#include <mutex>
#include <thread>

namespace pr::net {
namespace {
using tcp = boost::asio::ip::tcp;
struct ListenerState : std::enable_shared_from_this<ListenerState> {
    explicit ListenerState(std::function<boost::asio::any_io_executor()> next)
        : next_executor(std::move(next)), guard(boost::asio::make_work_guard(io)),
          tls(std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls_server)) {}
    boost::asio::io_context io;
    std::function<boost::asio::any_io_executor()> next_executor;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> guard;
    std::vector<std::shared_ptr<tcp::acceptor>> acceptors;
    std::vector<ListenerConfig> listeners;
    std::mutex tls_mutex;
    std::shared_ptr<boost::asio::ssl::context> tls;
    IServerTransport::AcceptHandler handler;
    std::atomic<bool> stopped{false};

    std::shared_ptr<boost::asio::ssl::context> tls_context() {
        std::lock_guard<std::mutex> lock(tls_mutex);
        return tls;
    }
    void accept(size_t index) {
        if (stopped.load()) return;
        auto self = shared_from_this();
        auto socket = std::make_unique<tcp::socket>(next_executor());
        auto* raw = socket.get();
        acceptors[index]->async_accept(*raw,
            [self, index, socket = std::move(socket)](const boost::system::error_code& error) mutable {
                if (self->stopped.load()) return;
                try {
                    if (!error) {
                        auto connection = self->listeners[index].type == ListenerType::SSL
                            ? make_asio_ssl_connection(std::move(socket), self->tls_context())
                            : make_asio_tcp_connection(std::move(socket));
                        self->handler(std::move(connection), self->listeners[index]);
                    } else {
                        LOG_NETWORK_WARN("Accept failed: {}", error.message());
                    }
                } catch (const std::exception& error) {
                    LOG_NETWORK_ERROR("Accepted connection setup failed: {}", error.what());
                } catch (...) {
                    LOG_NETWORK_ERROR("Accepted connection setup threw a non-standard exception");
                }
                self->accept(index);
            });
    }
    void close() {
        for (auto& acceptor : acceptors) {
            boost::system::error_code ignored;
            acceptor->close(ignored);
        }
        handler = {};
        guard.reset();
    }
};

class AsioServerTransport final : public IServerTransport {
public:
    AsioServerTransport(const ServerTransportConfig& config,
                        std::function<boost::asio::any_io_executor()> next)
        : state_(std::make_shared<ListenerState>(std::move(next))) {
        const auto address = asio_call("parse listen address", [&] {
            return boost::asio::ip::make_address(config.address);
        });
        bool needs_tls = !config.tls.certificate_file.empty() || !config.tls.private_key_file.empty();
        for (const auto& listener : config.listeners) needs_tls |= listener.type == ListenerType::SSL;
        if (needs_tls) configure_tls(config.tls);
        for (auto listener : config.listeners) {
            auto acceptor = std::make_shared<tcp::acceptor>(state_->io);
            asio_call("bind listener", [&] {
                tcp::endpoint endpoint(address, listener.port);
                acceptor->open(endpoint.protocol());
                acceptor->set_option(tcp::acceptor::reuse_address(true));
                acceptor->bind(endpoint);
                acceptor->listen();
                listener.port = acceptor->local_endpoint().port();
            });
            state_->listeners.push_back(listener);
            state_->acceptors.push_back(std::move(acceptor));
        }
    }
    ~AsioServerTransport() override { stop(); }
    const std::vector<ListenerConfig>& listeners() const override { return state_->listeners; }
    void configure_tls(const TlsConfig& config) override {
        auto context = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls_server);
        asio_call("configure TLS listener", [&] {
            context->use_certificate_chain_file(config.certificate_file);
            context->use_private_key_file(config.private_key_file, boost::asio::ssl::context::pem);
            if (!config.enable_tls1_3) context->set_options(boost::asio::ssl::context::no_tlsv1_3);
            if (!config.dh_file.empty() && std::filesystem::exists(config.dh_file))
                context->use_tmp_dh_file(config.dh_file);
        });
        std::lock_guard<std::mutex> lock(state_->tls_mutex);
        state_->tls = std::move(context);
    }
    std::unique_ptr<IConnection> upgrade_tls(std::unique_ptr<IConnection> connection) override {
        return asio_call("upgrade TLS connection", [&] {
            return upgrade_asio_tls(std::move(connection), state_->tls_context());
        });
    }
    void start(AcceptHandler handler) override {
        std::lock_guard<std::mutex> lock(lifecycle_);
        if (started_) return;
        if (state_->stopped.load()) throw std::logic_error("listener has already stopped");
        state_->handler = std::move(handler);
        auto state = state_;
        boost::asio::post(state->io, [state] {
            for (size_t i = 0; i < state->acceptors.size(); ++i) state->accept(i);
        });
        thread_ = std::thread([state] {
            for (;;) {
                try { state->io.run(); break; }
                catch (const std::exception& error) { LOG_NETWORK_ERROR("Listener callback threw: {}", error.what()); }
                catch (...) { LOG_NETWORK_ERROR("Listener callback threw a non-standard exception"); }
            }
        });
        started_ = true;
    }
    void stop() override {
        std::lock_guard<std::mutex> lock(lifecycle_);
        if (state_->stopped.exchange(true)) return;
        if (thread_.joinable()) {
            if (thread_.get_id() == std::this_thread::get_id()) {
                // The final shared owner can be released inside an accept
                // callback. Its loop owns only ListenerState, never this.
                state_->close();
                thread_.detach();
                return;
            }
            // Closing acceptors and removing the callback on the listener thread
            // also serializes shutdown against a ready successful accept.
            auto state = state_;
            boost::asio::post(state->io, [state] { state->close(); });
            thread_.join();
        } else {
            state_->close();
            state_->io.stop();
        }
    }
private:
    std::shared_ptr<ListenerState> state_;
    std::mutex lifecycle_;
    std::thread thread_;
    bool started_ = false;
};
} // namespace
std::unique_ptr<IServerTransport> make_asio_server_transport(const ServerTransportConfig& config,
    std::function<boost::asio::any_io_executor()> next_executor) {
    return asio_call("create server transport", [&] {
        return std::make_unique<AsioServerTransport>(config, std::move(next_executor));
    });
}
} // namespace pr::net
