#ifndef MOCK_OUTBOUND_STREAM_H
#define MOCK_OUTBOUND_STREAM_H

#include <boost/asio.hpp>
#include <cstring>
#include <string>
#include <vector>

namespace mail_system {
namespace test {

// 模拟出站 SMTP 连接 —— 预填响应，捕获发送，不涉及真实网络。
// 配合 smtp_transport_utils 的 run_interruptible_ec / read_smtp_response 使用。
class MockOutboundStream {
public:
    MockOutboundStream() : io_ctx_(), read_pos_(0), eof_(false) {}

    // --- 数据注入 ---
    // 设置服务端会返回的 SMTP 响应行（含多行响应则分段）
    void set_responses(const std::vector<std::string>& lines) {
        read_buf_.clear();
        for (auto& l : lines) read_buf_ += l + "\r\n";
        read_pos_ = 0;
        eof_ = false;
    }

    // 追加更多响应（用于中途模拟）
    void append_response(const std::string& line) {
        read_buf_ += line + "\r\n";
    }

    // --- 读取：模拟 async_read_some，供 async_read_until 使用 ---
    const std::string& written() const { return write_buf_; }
    void clear_written() { write_buf_.clear(); }

    // --- asio 接口 ---
    using executor_type = boost::asio::io_context::executor_type;
    executor_type get_executor() { return io_ctx_.get_executor(); }

    // lowest_layer: yield *this (用于非 SSL 路径)
    MockOutboundStream& lowest_layer() { return *this; }

    template <typename MutableBufferSequence, typename ReadHandler>
    void async_read_some(MutableBufferSequence bufs, ReadHandler handler) {
        auto buf = boost::asio::buffer(bufs);
        size_t avail = read_buf_.size() - read_pos_;
        size_t n = std::min(buf.size(), avail);
        if (n > 0) {
            std::memcpy(buf.data(), read_buf_.data() + read_pos_, n);
            read_pos_ += n;
        }
        // 数据耗尽则返回 EOF，否则返回成功
        auto ec = (n == 0 && !eof_) ? boost::asio::error::eof : boost::system::error_code{};
        if (n == 0 && !eof_) eof_ = true;
        boost::asio::post(io_ctx_, [handler = std::move(handler), ec, n]() mutable {
            handler(ec, n);
        });
    }

    template <typename ConstBufferSequence, typename WriteHandler>
    void async_write_some(ConstBufferSequence bufs, WriteHandler handler) {
        size_t total = 0;
        for (auto it = boost::asio::buffer_sequence_begin(bufs);
             it != boost::asio::buffer_sequence_end(bufs); ++it) {
            auto b = *it;
            write_buf_.append(static_cast<const char*>(b.data()), b.size());
            total += b.size();
        }
        boost::asio::post(io_ctx_, [handler = std::move(handler), ec = boost::system::error_code{}, total]() mutable {
            handler(ec, total);
        });
    }

    template <typename ConnectHandler>
    void async_connect(const boost::asio::ip::tcp::endpoint&, ConnectHandler handler) {
        boost::asio::post(io_ctx_, [handler = std::move(handler)]() mutable {
            handler(boost::system::error_code{});
        });
    }

    void cancel(boost::system::error_code& ec) { ec.clear(); }
    void close(boost::system::error_code& ec) { ec.clear(); }

    // polling 辅助 —— run_interruptible_ec 依赖 io.restart + io.poll
    boost::asio::io_context& io() { return io_ctx_; }

private:
    boost::asio::io_context io_ctx_;
    std::string read_buf_;
    size_t read_pos_;
    bool eof_;
    std::string write_buf_;
};

} // namespace test
} // namespace mail_system

#endif // MOCK_OUTBOUND_STREAM_H
