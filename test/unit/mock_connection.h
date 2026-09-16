#ifndef MOCK_CONNECTION_H
#define MOCK_CONNECTION_H
#include "framework/connection/i_connection.h"
#include "framework/session_base.h"   // finish_session 需要 SessionBase 完整类型
#include "mock_io_context.h"

#include <boost/asio/io_context.hpp>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mail_system {

// ================================================================
// MockConnection — 基于 MockIoContext 的零 I/O 模拟连接
//
//   异步读/写完成被包装成任务 post 到内部 MockIoContext：
//     - 同步模式（默认，未 start()）：任务内联执行 → 与旧行为一致，
//       供串行 FSM 逻辑测试（process_event 同步完成）。
//     - 线程模式（start() 后）：任务由独立工作线程执行 → 还原 asio 的
//       completion 投递语义，供并发/异步测试（跨线程投递用户缓冲区）。
// ================================================================
class MockConnection : public IConnection {
public:
    MockConnection() = default;
    ~MockConnection() { stop_executor(); }

    // ---- MockIoContext 生命周期 ----
    void start() { ctx_.start(); }
    void stop() { ctx_.stop(); }
    test::MockIoContext& context() { return ctx_; }
    bool wait_idle(int timeout_ms = 3000) { return ctx_.wait_idle(timeout_ms); }

    // ---- exec_ctx_(真实 io_context)后台运行 ----
    // SessionBase 09-06 起 do_async_write/close 等发起都 post 到 get_executor()
    // (= exec_ctx_)。不跑它,发起队列饿死:响应永远上不了 wire。线程模式测试
    // (start() 后)调用此方法让 executor 有人跑;析构自动停。
    void start_executor() {
        if (exec_thread_.joinable()) return;
        exec_thread_ = std::thread([ctx = exec_ctx_]() {
            // guard 放线程栈:run() 无任务不返回;stop() 后随栈帧销毁
            auto guard = boost::asio::make_work_guard(*ctx);
            ctx->run();
        });
    }
    void stop_executor() {
        if (!exec_thread_.joinable()) return;
        if (exec_thread_.get_id() == std::this_thread::get_id()) {
            // 在 exec 线程内析构(回调持 session 最后引用):不能 join 自己。
            // ctx 由线程 lambda 的 shared_ptr 持有,detach 后 run() 排空自然释放。
            exec_thread_.detach();
            return;
        }
        exec_ctx_->stop();
        exec_thread_.join();
        exec_ctx_->restart();
    }

    // --- data injection ---
    void set_read_data(const std::string& data) {
        std::lock_guard<std::mutex> lk(mu_);
        read_buf_ = data;
        read_pos_ = 0;
    }
    void append_read_data(const std::string& data) {
        std::lock_guard<std::mutex> lk(mu_);
        read_buf_ += data;
    }
    std::string written() const {
        std::lock_guard<std::mutex> lk(mu_);
        return write_buf_;
    }
    void clear_written() {
        std::lock_guard<std::mutex> lk(mu_);
        write_buf_.clear();
    }
    void set_closed(bool c) {
        std::lock_guard<std::mutex> lk(mu_);
        closed_ = c;
    }

    // 将 async_write 的数据同步捕获到外部 string（用于 STARTTLS 等连接被释放后的断言）
    void capture_to(std::string* target) {
        std::lock_guard<std::mutex> lk(mu_);
        capture_target_ = target;
    }

    // ---- IDLE / deferred read support ----
    // 无数据时保存 {缓冲, handler}，由 trigger_deferred_read 投递数据。
    void set_deferred_read(bool v) {
        std::lock_guard<std::mutex> lk(mu_);
        deferred_read_ = v;
    }
    bool has_pending_read() const {
        std::lock_guard<std::mutex> lk(mu_);
        return static_cast<bool>(pending_read_.h);
    }
    void trigger_deferred_read(const std::string& data) {
        PendingRead pr;
        {
            std::lock_guard<std::mutex> lk(mu_);
            read_buf_ += data;
            pr = std::move(pending_read_);
            pending_read_ = PendingRead{};
        }
        if (!pr.h) return;
        ctx_.post([this, pr = std::move(pr)]() mutable {
            boost::system::error_code ec;
            size_t n = 0;
            {
                std::lock_guard<std::mutex> lk(mu_);
                size_t avail = read_buf_.size() - read_pos_;
                n = std::min(pr.buf.size(), avail);
                if (n > 0) {
                    std::memcpy(pr.buf.data(), read_buf_.data() + read_pos_, n);
                    read_pos_ += n;
                } else {
                    ec = boost::asio::error::eof;
                }
            }
            pr.h(ec, n);
        });
    }

    // ---- deferred write support（模拟慢写 / 写完成晚于 session 逻辑结束） ----
    // 数据仍立即写入 write_buf_，仅延迟完成回调；默认关闭（同步回调）。
    void set_deferred_write(bool v) {
        std::lock_guard<std::mutex> lk(mu_);
        deferred_write_ = v;
    }
    bool has_pending_write() const {
        std::lock_guard<std::mutex> lk(mu_);
        return pending_write_handler_ != nullptr;
    }
    void trigger_deferred_write() {
        WriteHandler h;
        {
            std::lock_guard<std::mutex> lk(mu_);
            h = std::move(pending_write_handler_);
            pending_write_handler_ = nullptr;
        }
        if (h) h(boost::system::error_code(), 0);
    }

    // --- IConnection impl ---
    void async_read(boost::asio::mutable_buffer buf, ReadHandler h) override {
        bool defer = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (deferred_read_ && read_buf_.size() <= read_pos_) {
                pending_read_ = PendingRead{buf, std::move(h)};
                defer = true;
            }
        }
        if (defer) return;

        ctx_.post([this, buf, h = std::move(h)]() mutable {
            boost::system::error_code ec;
            size_t n = 0;
            {
                std::lock_guard<std::mutex> lk(mu_);
                size_t avail = read_buf_.size() - read_pos_;
                if (avail > 0) {
                    n = std::min(buf.size(), avail);
                    // 工作线程把用户缓冲区内容投递到调用方缓冲（跨线程）
                    std::memcpy(buf.data(), read_buf_.data() + read_pos_, n);
                    read_pos_ += n;
                } else {
                    ec = boost::asio::error::eof;
                }
            }
            h(ec, n);
        });
    }

    boost::asio::any_io_executor get_executor() override {
        // watchdog（SessionBase::rearm/disarm_timeout）与发起序列化（09-06）的
        // socket 发起都 post 到这里。返回真实 io_context executor（自定义内联
        // executor 不满足 any_io_executor 的属性要求）——post 进队但测试不
        // run()，需要观察发起效果的用例调用 pump_executor() 手动排空。
        return boost::asio::any_io_executor{exec_ctx_->get_executor()};
    }

    // 排空发起队列：SessionBase 09-06 起 socket 发起经 post 串行化，单测
    // 无常驻 IO 线程，用例在 do_async_read/write 之后调用本方法驱动发起。
    //
    // 【会话收尾契约 — 违反即 LSan 报 indirect leak（CI asan-unit 全红）】
    // 排空必须发生在最后一个 session 引用被释放之前。队列里那些捕获 self 的
    // 发起 lambda（close / rearm / do_async_* 的 post）自己就是引用：没人执行
    // 它们时，session → connection_ → exec_ctx_ → 队列 lambda → session 构成
    // 不可达环，整棵对象图（含 FSM 转移表、handler 表、缓冲区）都归 LSan 的
    // indirect leak 名下，且看不到任何 direct leak —— 这就是"纯环"的特征。
    // 收尾姿势：close() 之后调一次本方法；fixture 统一放在 Handle 析构里。
    // poll() 会连续执行到无可就绪任务，故一次调用即排空当前队列及其链式投递；
    // 但已武装的 watchdog async_wait（handler 强持 self）只有 close() 能断，
    // 所以收尾必须是"close + 排空"，缺一不可。
    void pump_executor() {
        // poll 发现无任务时 io_context 会自动进入 stopped 态，之后的 post
        // 全部静默空转——restart 后才能继续驱动后续发起。
        if (exec_ctx_->stopped()) exec_ctx_->restart();
        exec_ctx_->poll();
    }

    void async_write(boost::asio::const_buffer buf, WriteHandler h) override {
        size_t n = buf.size();
        {
            std::lock_guard<std::mutex> lk(mu_);
            write_buf_.append(static_cast<const char*>(buf.data()), n);
            if (capture_target_) *capture_target_ = write_buf_;
            if (deferred_write_) {
                pending_write_handler_ = std::move(h);
                return;   // 延迟完成回调，由 trigger_deferred_write() 触发
            }
        }
        ctx_.post([h = std::move(h), n]() mutable {
            h(boost::system::error_code(), n);
        });
    }

    void async_write_with_delay(boost::asio::const_buffer buf,
                                std::chrono::milliseconds delay,
                                WriteHandler h) override {
        // Mock 无真实定时器：忽略 delay，按普通异步写投递
        (void)delay;
        async_write(buf, std::move(h));
    }

    void async_handshake(boost::asio::ssl::stream_base::handshake_type, HandshakeHandler h) override {
        ctx_.post([h = std::move(h)]() mutable {
            h(boost::system::error_code());
        });
    }

    void close() override {
        {
            std::lock_guard<std::mutex> lk(mu_);
            closed_ = true;
            // 还原 asio 语义：关闭连接释放挂起的读/写 handler。
            // 否则 pending_read_ 里捕获 shared_from_this 的 handler 与 session 形成
            // 引用环，session 永不析构 → LSan 泄漏（如 IMAP IDLE 的 deferred read）。
            pending_read_ = PendingRead{};
            pending_write_handler_ = nullptr;
        }
        // 驱动 watchdog executor 队列：rearm 的 post 捕获 session shared_ptr，
        // 若不排空，session 关闭后仍被队列钉住 → weak 永不过期/泄漏。
        // closed_ 已置位，排空只是让 rearm lambda 进去短路返回，不会武装定时器。
        exec_ctx_->poll();
    }
    bool is_open() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return !closed_;
    }
    uint16_t get_local_port() const override { return 0; }
    std::string get_remote_ip() const override { return "127.0.0.1"; }
    std::unique_ptr<boost::asio::ip::tcp::socket> release_socket() override { return nullptr; }

private:
    // deferred read：保存缓冲视图 + handler（缓冲指向 session 的 read_buffer_）
    struct PendingRead {
        boost::asio::mutable_buffer buf;
        ReadHandler h;
    };

    // ── 双 context 职责契约（09-06 单元层全真 asio 化定稿）──
    // exec_ctx_（真实 io_context）＝【调度语义层】：SessionBase 的发起/关闭
    //   串行队列、watchdog 定时器。单元层测试用 pump_executor()（restart+poll）
    //   驱动，与生产 IO 线程的 run() 走完全相同的 scheduler 语义。
    // ctx_（MockIoContext）＝【完成投递层】：mock 的 async_read/write 把完成
    //   回调投到这里，同步模式内联执行（测试即发起即生效）、线程模式由独立
    //   线程投递（smtps_fsm_concurrency_test 依赖 wait_idle 的确定性排空）。
    //   它不是 scheduler 仿真——不要用它测调度行为，那属于 exec_ctx_。
    mutable std::mutex mu_;
    test::MockIoContext ctx_;
    // 堆持有:允许 MockConnection 在 exec 线程内析构(close_after_flush 的 post
    // 持有 session 最后引用)时,ctx 由分离线程的 shared_ptr 续命,避免悬垂
    std::shared_ptr<boost::asio::io_context> exec_ctx_ =
        std::make_shared<boost::asio::io_context>();
    std::thread exec_thread_;   // 跑 exec_ctx_(guard 在线程栈上,见 start_executor)
    std::string read_buf_;
    size_t read_pos_ = 0;
    std::string write_buf_;
    bool closed_ = false;
    std::string* capture_target_ = nullptr;
    bool deferred_read_ = false;
    bool deferred_write_ = false;
    PendingRead pending_read_;
    WriteHandler pending_write_handler_;
};

// ================================================================
// finish_session — 单测会话收尾（会话收尾契约的落地，见 pump_executor 注释）
//
//   关闭会话 → 排空发起队列 → 等引用计数收敛到"只剩调用方这一份"。
//
//   为什么不能只排空一次：close() 常由 worker 线程的异步续作触发（DB 查询
//   回调、POP3 心跳续约失败回调等），这些续作的后续 post 会晚于本次排空
//   几十微秒到达 —— 单次排空必漏，表现为 LSan 偶发报 indirect leak
//   （pop3_fsm_test 心跳用例：6 次跑 2 次红，且每次泄漏字节数完全相同）。
//
//   use_count() 是"还有谁持有会话"的直接观测量：exec_ctx_ 队列里的发起
//   lambda、watchdog 的 async_wait handler、worker 回调各自持一份 self。
//   计数收敛到 1 即意味着除调用方外再无持有者，此时释放调用方那一份，
//   session → connection_ → exec_ctx_ 整图归零。
//
//   约定：调用点应是该会话的最后一个持有者（fixture 的 Handle 成员、或
//   测试块里的局部 shared_ptr）；若测试另存了引用，本函数会白等到上限
//   （不致命，只是慢）。不可用于已 release_connection() 的会话
//   （get_connection() 解引用空指针）。
// ================================================================
template <typename SessionPtr>
inline void finish_session(SessionPtr& session, int timeout_ms = 400) {
    if (!session) return;
    if (!session->is_closed()) {
        session->set_trace_clean_close();   // 单测无需落 trace 文件
        session->close();
    }
    for (int waited = 0; waited < timeout_ms && session.use_count() > 1; waited += 2) {
        session->get_connection().pump_executor();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    session->get_connection().pump_executor();
}

} // namespace mail_system
#endif
