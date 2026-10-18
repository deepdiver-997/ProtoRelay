#pragma once
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <cassert>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace smtp_test {
namespace asio = boost::asio;
using asio::ip::tcp;
struct Step {
    std::string expected, reply;
    bool data = false, disconnect = false, upgrade = false;
};
// 真实 TCP/TLS 消费者；逐命令/完整 DATA 读取，可分片响应和中断最终应答。
class Peer : public std::enable_shared_from_this<Peer> {
public:
    Peer(asio::io_context& io, std::vector<Step> steps, std::string greeting = "220 test.local\r\n",
         std::shared_ptr<asio::ssl::context> tls = {}, bool implicit = false, bool fragment = true)
        : acceptor_(io, {asio::ip::address_v4::loopback(), 0}), socket_(io), steps_(std::move(steps)),
          greeting_(std::move(greeting)), context_(std::move(tls)), implicit_(implicit), fragment_(fragment) {}
    uint16_t port() const { return acceptor_.local_endpoint().port(); }
    size_t completed = 0;
    bool accepted = false;
    void start() {
        auto self = shared_from_this();
        acceptor_.async_accept(socket_, [self](auto ec) {
            if (ec) return;
            self->accepted = true;
            if (self->implicit_) self->handshake(true);
            else if (!self->greeting_.empty()) self->write(self->greeting_);
        });
    }
    void stop() {
        boost::system::error_code ignored;
        acceptor_.close(ignored);
        if (tls_) tls_->next_layer().close(ignored);
        else socket_.close(ignored);
    }
private:
    void handshake(bool initial) {
        tls_ = std::make_unique<asio::ssl::stream<tcp::socket>>(std::move(socket_), *context_);
        auto self = shared_from_this();
        tls_->async_handshake(asio::ssl::stream_base::server, [self, initial](auto ec) {
            // 负例允许客户端因证书验证失败而断开。
            if (ec) return;
            if (initial) self->write(self->greeting_);
            else self->read();
        });
    }
    void write(std::string wire, bool upgrade = false) {
        wire_ = std::move(wire); offset_ = 0; upgrade_ = upgrade;
        write_chunk();
    }
    void write_chunk() {
        const size_t count = fragment_ ? std::min<size_t>(3, wire_.size() - offset_) : wire_.size() - offset_;
        auto self = shared_from_this();
        auto cb = [self](auto ec, size_t count) {
            if (ec) return;
            self->offset_ += count;
            if (self->offset_ < self->wire_.size()) self->write_chunk();
            else if (self->upgrade_) self->handshake(false);
            else self->read();
        };
        const auto buffer = asio::buffer(wire_.data() + offset_, count);
        if (tls_) asio::async_write(*tls_, buffer, cb);
        else asio::async_write(socket_, buffer, cb);
    }
    void read() {
        if (completed == steps_.size()) return;
        auto self = shared_from_this();
        auto cb = [self](auto ec, size_t count) {
            if (ec) return;
            std::string wire(count, '\0');
            std::istream input(&self->input_); input.read(wire.data(), count);
            const auto& step = self->steps_[self->completed++];
            if (wire != step.expected) {
                std::cerr << "unexpected SMTP wire at step " << self->completed << ": " << wire << std::endl;
                assert(wire == step.expected);
            }
            if (step.disconnect) { self->stop(); return; }
            if (!step.reply.empty()) self->write(step.reply, step.upgrade);
        };
        const char* delimiter = steps_[completed].data ? "\r\n.\r\n" : "\r\n";
        if (tls_) asio::async_read_until(*tls_, input_, delimiter, cb);
        else asio::async_read_until(socket_, input_, delimiter, cb);
    }
    tcp::acceptor acceptor_;
    tcp::socket socket_;
    std::vector<Step> steps_;
    std::string greeting_, wire_;
    std::shared_ptr<asio::ssl::context> context_;
    std::unique_ptr<asio::ssl::stream<tcp::socket>> tls_;
    asio::streambuf input_;
    size_t offset_ = 0;
    bool implicit_, fragment_, upgrade_ = false;
};
} // namespace smtp_test
