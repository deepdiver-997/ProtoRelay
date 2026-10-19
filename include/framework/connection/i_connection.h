#ifndef PR_FRAMEWORK_I_CONNECTION_H
#define PR_FRAMEWORK_I_CONNECTION_H

#include "framework/net/io_types.h"
#include "framework/net/executor.h"
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace pr {

template <typename ConnectionType>
class SessionBase;

// Backend-independent connection contract. Initiate IO, close, and cancel on
// executor(); completions run on that same serialized executor. Buffers and
// the connection must remain alive through completion. Reads return available
// bytes; successful writes send the entire buffer. Allow at most one pending
// read and one pending write: serialized initiation does not serialize entire
// asynchronous operations. EOF is net::IoErrc::end_of_stream,
// cancellation is std::errc::operation_canceled.
class IConnection {
public:
    using ReadHandler = std::function<void(const std::error_code&, std::size_t)>;
    using WriteHandler = std::function<void(const std::error_code&, std::size_t)>;
    using HandshakeHandler = std::function<void(const std::error_code&)>;

    virtual ~IConnection() = default;

    // 异步读取
    virtual void async_read(
        net::MutableBuffer buffer,
        ReadHandler handler
    ) = 0;

    // 获取 executor，用于创建定时器等
    virtual std::shared_ptr<net::IExecutor> executor() = 0;

    // 异步写入
    virtual void async_write(
        net::ConstBuffer buffer,
        WriteHandler handler
    ) = 0;

    // 异步延时写入（用 steady_timer 实现非阻塞延时）
    virtual void async_write_with_delay(
        net::ConstBuffer buffer,
        std::chrono::milliseconds delay,
        WriteHandler handler)
    {
        if (delay.count() == 0) {
            async_write(buffer, std::move(handler));
            return;
        }
        auto timer = executor()->make_timer();
        timer->expires_after(delay);
        timer->async_wait([this, buffer, handler = std::move(handler), timer](const std::error_code& ec) mutable {
            if (ec) {
                handler(ec, 0);
                return;
            }
            async_write(buffer, std::move(handler));
        });
    }

    // SSL 握手（TCP 连接直接调用成功回调）
    virtual void async_handshake(
        net::HandshakeRole type,
        HandshakeHandler handler
    ) = 0;

    // 关闭连接
    virtual void close() = 0;

    // 取消挂起的异步操作（尤其是挂起的 async_read）。不关闭连接；
    // 被取消的 op 的 handler 以 operation_aborted 立即返回。
    // Watchdog 超时回收挂死连接靠它强行制造一个 io 完成点。
    virtual void cancel() {}   // 默认 no-op（有的连接类型无挂起 op 也安全）

    // 检查连接是否打开
    virtual bool is_open() const = 0;

    // 获取本地端口（隐藏底层细节）
    virtual uint16_t get_local_port() const = 0;

    // 获取远程 IP 地址
    virtual std::string get_remote_ip() const = 0;

    // 握手失败诊断信息（SSL 连接返回 OpenSSL 错误栈/状态；TCP 连接返回空）
    virtual std::string get_handshake_diagnostic() const { return {}; }

    // 原生 fd（Linux 零拷贝 sendfile/splice 需要）。默认 -1 表示不可用；
    // TcpConnection 覆写返回真实 fd。注意：该 fd 绕过 SSL 层，绝不可用于 TLS。
    virtual int native_handle() const { return -1; }
};

} // namespace pr

#endif // PR_FRAMEWORK_I_CONNECTION_H
