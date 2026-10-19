#ifndef PR_FRAMEWORK_LOG_LOG_PURE_H
#define PR_FRAMEWORK_LOG_LOG_PURE_H

#include "framework/log/log_format.h"
#include <cstdio>
#include <chrono>
#include <string>
#include <filesystem>
#include <utility>
#include <iomanip>
#include <mutex>

namespace pr {

// ================================================================
// LOG_PURE — 日志压缩运行时
//
// 配合 tools/log_transform.py 使用。
// 构建时脚本将 LOG_* 宏替换为 LOG_PURE(hash, args..., ts)。
// 运行时输出 "0xhash|arg1|...|ts_ms\n" 到专用纯日志文件。
//
// hash = shake128([LEVEL][MODULE]fmt) — 稳定，不含文件/行号
// 映射表增量维护，后处理用 tools/log_restore.py 还原可读日志。
// ================================================================

/** 获取当前毫秒时间戳，LOG_PURE 自动注入 */
inline uint64_t log_pure_timestamp_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

namespace detail {
struct PureLogState {
    std::mutex mutex;
    std::string path = "logs/pure.log";
    FILE* file = nullptr;
    ~PureLogState() { if (file) std::fclose(file); }
};
inline PureLogState& pure_log_state() { static PureLogState state; return state; }
}

/** 设置纯日志输出文件路径 (须在首次写日志前调用) */
inline void log_pure_init(const std::string& path = "logs/pure.log") {
    auto& state = detail::pure_log_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.file) std::fclose(state.file);
    state.file = nullptr;
    state.path = path;
}

/** 写纯日志行: "0xhash|arg1|...|argN\n" (行缓冲, 崩溃不丢) */
template <typename... Args>
inline void log_pure_write(uint64_t hash, Args&&... args) {
    std::ostringstream line;
    line.imbue(std::locale::classic());
    line << "0x" << std::hex << std::setw(16) << std::setfill('0') << hash;
    ((line << '|' << detail::log_argument(std::forward<Args>(args))), ...);
    line << '\n';
    const auto text = line.str();
    auto& state = detail::pure_log_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.file) {
        std::filesystem::path p(state.path);
        if (!p.parent_path().empty())
            std::filesystem::create_directories(p.parent_path());
        state.file = std::fopen(state.path.c_str(), "a");
        if (state.file) std::setvbuf(state.file, nullptr, _IOLBF, 0);
    }
    if (state.file) std::fwrite(text.data(), 1, text.size(), state.file);
}

inline void log_pure_flush() {
    auto& state = detail::pure_log_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.file) std::fflush(state.file);
}

} // namespace pr

// ---- 宏定义 ----

// LOG_PURE(hash, args...) → "0xhash|arg1|...|ts\n"
#define LOG_PURE(hash, ...) \
    pr::log_pure_write(hash, ##__VA_ARGS__)

// ================================================================
// LOG_LOOK_UP 模式 —— 预处理器分析阶段
//
// 当通过 -DLOG_LOOK_UP 编译时，所有 LOG_* 宏展开为带有 \001 分隔符
// 的标记字符串。tools/log_transform.py 运行 gcc -E 解析这些标记。
// ================================================================
#ifdef LOG_LOOK_UP
#define __LOG_MARK(module, level, fmt, ...) \
    (void)("LUK" "\001" module "\001" level "\001" fmt)
#else
#define __LOG_MARK(module, level, fmt, ...)  /* no-op in normal mode */
#endif

#endif // PR_FRAMEWORK_LOG_LOG_PURE_H
