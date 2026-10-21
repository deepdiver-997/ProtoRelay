#include "framework/net/asio_client_transport.h"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <openssl/ssl.h>
#include <stdexcept>

namespace pr::net {
namespace {
namespace asio = boost::asio;
using tcp = asio::ip::tcp;
class AsioClientTransport final : public IClientTransport {
    struct State {
        explicit State(const asio::any_io_executor& ex) : socket(ex), resolver(ex) {}
        bool closed = false;
        tcp::socket socket;
        tcp::resolver resolver;
        std::shared_ptr<asio::ssl::context> context;
        std::unique_ptr<asio::ssl::stream<tcp::socket>> tls;
        tcp::socket& lowest() { return tls ? tls->next_layer() : socket; }
        void close() {
            closed = true;
            resolver.cancel();
            boost::system::error_code ignored;
            lowest().close(ignored);
        }
    };
public:
    explicit AsioClientTransport(std::shared_ptr<AsioExecutor> executor) : executor_(std::move(executor)) {
        if (!executor_) throw std::invalid_argument("client transport requires an executor");
    }
    std::shared_ptr<IExecutor> executor() override { return executor_; }
    void async_connect(const std::string& host, uint16_t port, HandshakeHandler handler) override {
        close();
        auto state = std::make_shared<State>(executor_->native_executor());
        state_ = state;
        state->resolver.async_resolve(host, std::to_string(port),
            [state, handler = std::move(handler)](const boost::system::error_code& error,
                                                tcp::resolver::results_type endpoints) mutable {
                if (state->closed) { handler(std::make_error_code(std::errc::operation_canceled)); return; }
                if (error) { handler(asio_error(error)); return; }
                asio::async_connect(state->socket, endpoints,
                    [state, handler = std::move(handler)](const boost::system::error_code& error, tcp::endpoint) {
                        handler(state->closed ? std::make_error_code(std::errc::operation_canceled) : asio_error(error));
                    });
            });
    }
    void async_start_tls(ClientTlsConfig config, HandshakeHandler handler) override {
        auto state = state_;
        if (!state || state->tls || config.server_name.empty()) {
            executor_->post([handler = std::move(handler)] {
                handler(std::make_error_code(std::errc::invalid_argument));
            });
            return;
        }
        try {
            state->context = std::make_shared<asio::ssl::context>(asio::ssl::context::tls_client);
            if (config.use_system_ca) state->context->set_default_verify_paths();
            if (!config.ca_file.empty()) state->context->load_verify_file(config.ca_file);
            if (!config.ca_pem.empty()) state->context->add_certificate_authority(asio::buffer(config.ca_pem));
            state->tls = std::make_unique<asio::ssl::stream<tcp::socket>>(std::move(state->socket), *state->context);
            if (!SSL_set_min_proto_version(state->tls->native_handle(), TLS1_2_VERSION))
                throw std::runtime_error("cannot set minimum TLS version");
            state->tls->set_verify_mode(asio::ssl::verify_peer);
            state->tls->set_verify_callback(asio::ssl::host_name_verification(config.server_name));
            if (!SSL_set_tlsext_host_name(state->tls->native_handle(), config.server_name.c_str()))
                throw std::runtime_error("cannot set TLS server name");
        } catch (const std::exception& error) {
            diagnostic_ = error.what();
            executor_->post([handler = std::move(handler)] {
                handler(std::make_error_code(std::errc::protocol_error));
            });
            return;
        }
        state->tls->async_handshake(asio::ssl::stream_base::client,
            [state, handler = std::move(handler)](const boost::system::error_code& error) { handler(asio_error(error)); });
    }
    void async_read(MutableBuffer buffer, ReadHandler handler) override {
        auto state = state_;
        if (!state) { complete_closed(std::move(handler)); return; }
        auto done = [state, handler = std::move(handler)](const boost::system::error_code& error, size_t size) {
            handler(asio_error(error), size);
        };
        if (state->tls) state->tls->async_read_some(asio::buffer(buffer.data(), buffer.size()), std::move(done));
        else state->socket.async_read_some(asio::buffer(buffer.data(), buffer.size()), std::move(done));
    }
    void async_write(ConstBuffer buffer, WriteHandler handler) override {
        auto state = state_;
        if (!state) { complete_closed(std::move(handler)); return; }
        auto done = [state, handler = std::move(handler)](const boost::system::error_code& error, size_t size) {
            handler(asio_error(error), size);
        };
        if (state->tls) asio::async_write(*state->tls, asio::buffer(buffer.data(), buffer.size()), std::move(done));
        else asio::async_write(state->socket, asio::buffer(buffer.data(), buffer.size()), std::move(done));
    }
    void async_handshake(HandshakeRole role, HandshakeHandler handler) override {
        // async_start_tls performs the only TLS handshake; an already established
        // client connection needs no second handshake.
        const bool connected = is_open();
        executor_->post([role, connected, handler = std::move(handler)] {
            handler(!connected ? std::make_error_code(std::errc::not_connected) :
                    role == HandshakeRole::client ? std::error_code{} :
                    std::make_error_code(std::errc::operation_not_supported));
        });
    }
    void cancel() override {
        if (!state_) return;
        state_->resolver.cancel();
        boost::system::error_code ignored;
        state_->lowest().cancel(ignored);
    }
    void close() override { if (state_) state_->close(); }
    bool is_open() const override { return state_ && state_->lowest().is_open(); }
    uint16_t get_local_port() const override {
        if (!state_) return 0;
        boost::system::error_code error;
        return state_->lowest().local_endpoint(error).port();
    }
    std::string get_remote_ip() const override {
        if (!state_) return {};
        boost::system::error_code error;
        auto endpoint = state_->lowest().remote_endpoint(error);
        return error ? std::string{} : endpoint.address().to_string();
    }
    std::string get_handshake_diagnostic() const override { return diagnostic_; }
private:
    void complete_closed(ReadHandler handler) {
        executor_->post([handler = std::move(handler)] { handler(std::make_error_code(std::errc::not_connected), 0); });
    }
    std::shared_ptr<AsioExecutor> executor_;
    std::shared_ptr<State> state_;
    std::string diagnostic_;
};
} // namespace
std::shared_ptr<IClientTransport> make_asio_client_transport(std::shared_ptr<AsioExecutor> executor) {
    return std::make_shared<AsioClientTransport>(std::move(executor));
}
} // namespace pr::net
