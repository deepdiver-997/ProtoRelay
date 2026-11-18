#ifndef PR_FRAMEWORK_STORAGE_I_WRITE_STREAM_H
#define PR_FRAMEWORK_STORAGE_I_WRITE_STREAM_H

#include "framework/storage/io_error.h"

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <functional>
#include <memory>
#include <utility>

namespace pr {

// 单个存储对象的写入句柄。
//
// 语义刻意照着 POSIX 的 pwrite 定，但只抄「显式」不抄「有状态」：
//   - 位置由调用方给出（write_at 的 offset），流内部不维护游标；
//     不存在 seek，也不存在 O_APPEND 那种「位置由被调用方决定」的语义。
//   - 调用方保证 offset 在 issue 时刻连续递增（单写者）。本地实现据此可用
//     pwrite，完成顺序无关紧要；仅支持追加的后端（HDFS/S3）也能按序落盘。
//   - commit() 是 POSIX 的 close() 所没有的概念：返回 true 才代表数据已持久化，
//     SMTP 侧只有在此之后才可以回 250，否则就是对发送方 MTA 撒谎。
//   - 错误一律携带 IoError：调用方据 retryable() 决定 451（重投）或 550（拒收）。
class IWriteStream {
public:
    virtual ~IWriteStream() = default;

    // 在 offset 处写入 size 字节。offset 必须等于此前已写入的总字节数。
    // 保持同步签名；本地 pwrite 通常写入 page cache，但内存压力或回写限流
    // 仍可能令它阻塞。成功不代表已持久化，持久化边界是 commit。
    virtual bool write_at(std::uint64_t offset,
                          const char* data,
                          std::size_t size,
                          IoError& error) = 0;

    // 冲刷并持久化。返回 true 后数据方可视为已落盘。
    virtual bool commit(IoError& error) = 0;

    // 异步形状的 commit，回调携带 (成功与否, 错误值)。
    //
    // 默认实现在发起线程内联执行；带 runtime 的本地流和远程包装可覆写为
    // 真异步。async 的命名本身不保证调用不阻塞。
    //
    // 回调线程契约：真异步实现允许在 provider 自己的线程触发回调。调用方
    // 必须已通过流水线 set_paused(true) 取得 session 的独占访问
    // （SPF/DNS 回调的同一约定），或自行把续作 post 回 io 线程。
    using CommitCallback = std::function<void(bool ok, const IoError& error)>;

    virtual void commit_async(CommitCallback cb) {
        IoError error;
        const bool ok = commit(error);
        if (cb) cb(ok, ok ? IoError{} : std::move(error));
    }

    using CompletionDispatcher = std::function<void(std::function<void()>)>;

    // 将完成交接到调用方的 executor；旧后端无需覆写。dispatcher 必须接受
    // closure 或抛异常，不能静默丢弃。持有 dispatcher 的 runtime 必须先排空
    // 提交再停止完成 executor。异常交接仅报告失败，不回退执行持久化。
    virtual void commit_async_on(CompletionDispatcher dispatcher, CommitCallback cb) {
        if (!dispatcher) {
            commit_async(std::move(cb));
            return;
        }
        commit_async([dispatcher = std::move(dispatcher), cb = std::move(cb)]
                     (bool ok, const IoError& error) mutable {
            dispatch_completion(std::move(dispatcher), std::move(cb), ok, error);
        });
    }

    // 放弃写入并清除半成品对象。析构路径会调用，不得抛异常。
    virtual void abort() noexcept = 0;

protected:
    static void dispatch_completion(CompletionDispatcher dispatcher, CommitCallback cb,
                                    bool ok, const IoError& error) {
        if (!dispatcher) {
            if (cb) cb(ok, error);
            return;
        }
        struct Completion {
            std::atomic<bool> delivered{false};
            CommitCallback callback;
            void finish(bool success, const IoError& result) {
                if (!delivered.exchange(true) && callback) callback(success, result);
            }
        };
        std::shared_ptr<Completion> completion;
        try {
            completion = std::make_shared<Completion>();
            completion->callback = std::move(cb);
            dispatcher([completion, ok, error] { completion->finish(ok, error); });
        } catch (...) {
            // Even allocating the handoff can fail; this fallback must not need
            // another string allocation or lose the original callback.
            IoError failure;
            failure.code = ECANCELED;
            if (completion) completion->finish(false, failure);
            else if (cb) cb(false, failure);
        }
    }
};

} // namespace pr

#endif // PR_FRAMEWORK_STORAGE_I_WRITE_STREAM_H
