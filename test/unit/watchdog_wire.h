// WatchdogWire — watchdog 专项测试连接（asio 语义忠实版）。
//
// 与 MockConnection 的唯一关键差异：cancel() 像 asio socket cancel 一样把挂起
// 的读以 std::errc::operation_canceled 完成（MockConnection 的 cancel 走基类
// no-op，close 才丢 handler）。没有这一步，"watchdog 到期 → cancel 挂起读 →
// 读完成回调见 close_requested_ → close"的对端断电回收链在单测里走不通
//（on_watchdog_fire 的 connection_->cancel() 落在 no-op 上，读永不完成）。
//
// 完成语义其余与契约一致（i_connection.h）：完成经 executor 投递、同 executor
// 串行、恰好一次、EOF 用 end_of_stream、取消用 operation_canceled。
#ifndef PR_TEST_WATCHDOG_WIRE_H
#define PR_TEST_WATCHDOG_WIRE_H

#include "framework/connection/i_connection.h"
#include <cassert>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

namespace mail_system::test {

class WatchdogWire final : public pr::IConnection {
public:
    explicit WatchdogWire(std::shared_ptr<pr::net::IExecutor> executor)
        : executor_(std::move(executor)) {
        assert(executor_);
    }

    // ── 数据注入 / 观测 ──────────────────────────────────────
    // 投递字节；有挂起读则按 asio 语义完成它（可跨线程调用）。
    void feed(const std::string& data) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            inbuf_ += data;
        }
        maybe_complete_pending();
    }

    std::string written() const {
        std::lock_guard<std::mutex> lk(mu_);
        return written_;
    }
    void clear_written() {
        std::lock_guard<std::mutex> lk(mu_);
        written_.clear();
    }
    int cancel_count() const {
        std::lock_guard<std::mutex> lk(mu_);
        return cancel_count_;
    }
    bool has_pending_read() const {
        std::lock_guard<std::mutex> lk(mu_);
        return pending_read_.h != nullptr;
    }

    // ── pr::IConnection ──────────────────────────────────────
    std::shared_ptr<pr::net::IExecutor> executor() override { return executor_; }

    void async_read(pr::net::MutableBuffer buf, ReadHandler h) override {
        {
            std::lock_guard<std::mutex> lk(mu_);
            assert(!pending_read_.h && "overlapping reads on watchdog wire");
            pending_read_ = PendingRead{buf, std::move(h)};
        }
        maybe_complete_pending();
    }

    void async_write(pr::net::ConstBuffer buf, WriteHandler h) override {
        const size_t n = buf.size();
        {
            std::lock_guard<std::mutex> lk(mu_);
            written_.append(static_cast<const char*>(buf.data()), n);
        }
        executor_->post([h = std::move(h), n] { h(std::error_code(), n); });
    }

    void async_handshake(pr::net::HandshakeRole, HandshakeHandler h) override {
        executor_->post([h = std::move(h)] { h(std::error_code()); });
    }

    void cancel() override {
        PendingRead p;
        {
            std::lock_guard<std::mutex> lk(mu_);
            ++cancel_count_;
            if (!pending_read_.h) return;      // 契约：无挂起 op 也安全
            p = std::move(pending_read_);
            pending_read_ = PendingRead{};
        }
        post_aborted(std::move(p));
    }

    void close() override {
        PendingRead p;
        {
            std::lock_guard<std::mutex> lk(mu_);
            closed_ = true;
            if (pending_read_.h) {             // 还原 asio：close 完成挂起 op
                p = std::move(pending_read_);
                pending_read_ = PendingRead{};
            }
        }
        if (p.h) post_aborted(std::move(p));
    }

    bool is_open() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return !closed_;
    }
    uint16_t get_local_port() const override { return 0; }
    std::string get_remote_ip() const override { return "127.0.0.1"; }

private:
    struct PendingRead {
        pr::net::MutableBuffer buf;
        ReadHandler h;
    };

    void post_aborted(PendingRead p) {
        executor_->post([p = std::move(p)] {
            p.h(std::make_error_code(std::errc::operation_canceled), 0);
        });
    }

    // 有挂起读且缓冲有数据 → 拷贝至多 buf.size() 字节、清空 pending、投递完成。
    void maybe_complete_pending() {
        PendingRead p;
        size_t n = 0;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!pending_read_.h || in_pos_ >= inbuf_.size()) return;
            n = std::min(inbuf_.size() - in_pos_, pending_read_.buf.size());
            std::memcpy(pending_read_.buf.data(), inbuf_.data() + in_pos_, n);
            in_pos_ += n;
            if (in_pos_ == inbuf_.size()) { inbuf_.clear(); in_pos_ = 0; }
            p = std::move(pending_read_);
            pending_read_ = PendingRead{};
        }
        executor_->post([p = std::move(p), n] { p.h(std::error_code(), n); });
    }

    std::shared_ptr<pr::net::IExecutor> executor_;
    mutable std::mutex mu_;
    std::string inbuf_;
    size_t in_pos_ = 0;
    std::string written_;
    PendingRead pending_read_;
    bool closed_ = false;
    int cancel_count_ = 0;
};

} // namespace mail_system::test

#endif // PR_TEST_WATCHDOG_WIRE_H
