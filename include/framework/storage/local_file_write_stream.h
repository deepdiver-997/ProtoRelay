#ifndef PR_FRAMEWORK_STORAGE_LOCAL_FILE_WRITE_STREAM_H
#define PR_FRAMEWORK_STORAGE_LOCAL_FILE_WRITE_STREAM_H

#include "framework/storage/i_write_stream.h"

#include <memory>
#include <string>

namespace pr {

class LocalStorageRuntime;

// 常开 fd + 显式 pwrite。异步 commit 独占提交阶段，abort 不会跨线程关闭
// 正在 pwrite/fsync 的 fd；已入队任务持有独立状态，不依赖外层流的生命周期。
class LocalFileWriteStream : public IWriteStream {
public:
    // Instance-local syscall seams for deterministic failure/concurrency tests.
    // Production providers leave both empty and always use real pwrite/fsync.
    struct Operations {
        std::function<std::ptrdiff_t(int, const char*, std::size_t, std::uint64_t)> pwrite;
        std::function<int(int)> fsync;
    };

    static std::unique_ptr<LocalFileWriteStream> open(
        const std::string& path, IoError& error,
        std::shared_ptr<LocalStorageRuntime> runtime = {}, Operations operations = {});

    ~LocalFileWriteStream() override;
    LocalFileWriteStream(const LocalFileWriteStream&) = delete;
    LocalFileWriteStream& operator=(const LocalFileWriteStream&) = delete;

    bool write_at(std::uint64_t offset, const char* data, std::size_t size,
                  IoError& error) override;
    bool commit(IoError& error) override;
    void commit_async(CommitCallback cb) override;
    void commit_async_on(CompletionDispatcher dispatcher, CommitCallback cb) override;
    void abort() noexcept override;

private:
    struct State;
    explicit LocalFileWriteStream(std::shared_ptr<State> state) : state_(std::move(state)) {}
    std::shared_ptr<State> state_;
};

} // namespace pr

#endif // PR_FRAMEWORK_STORAGE_LOCAL_FILE_WRITE_STREAM_H
