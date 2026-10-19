#ifndef PR_FRAMEWORK_CONNECTION_HANDLE_H
#define PR_FRAMEWORK_CONNECTION_HANDLE_H
#include "framework/connection/i_connection.h"
#include <stdexcept>

namespace pr {
// Stable protocol-facing connection. The owned implementation can come from
// any network backend; neither session templates nor factories need its type.
class ConnectionHandle : public IConnection {
public:
    explicit ConnectionHandle(std::unique_ptr<IConnection> transport)
        : transport_(std::move(transport)) {
        if (!transport_) throw std::invalid_argument("connection transport is required");
    }
    void async_read(net::MutableBuffer b, ReadHandler h) override { transport_->async_read(b, std::move(h)); }
    void async_write(net::ConstBuffer b, WriteHandler h) override { transport_->async_write(b, std::move(h)); }
    void async_handshake(net::HandshakeRole role, HandshakeHandler h) override { transport_->async_handshake(role, std::move(h)); }
    std::shared_ptr<net::IExecutor> executor() override { return transport_->executor(); }
    void close() override { if (transport_) transport_->close(); }
    void cancel() override { if (transport_) transport_->cancel(); }
    bool is_open() const override { return transport_ && transport_->is_open(); }
    uint16_t get_local_port() const override { return transport_->get_local_port(); }
    std::string get_remote_ip() const override { return transport_->get_remote_ip(); }
    std::string get_handshake_diagnostic() const override { return transport_->get_handshake_diagnostic(); }
    int native_handle() const override { return transport_ ? transport_->native_handle() : -1; }
    std::unique_ptr<IConnection> release_transport() { return std::move(transport_); }
private:
    std::unique_ptr<IConnection> transport_;
};

inline std::unique_ptr<IConnection> take_connection_transport(std::unique_ptr<IConnection> connection) {
    while (auto* handle = dynamic_cast<ConnectionHandle*>(connection.get()))
        connection = handle->release_transport();
    return connection;
}
} // namespace pr
#endif
