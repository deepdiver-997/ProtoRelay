#!/bin/bash
# 构建脚本 - 支持 Debug / Release / SafeRelease 模式
# 交叉编译: cross-x64 模式自动处理 sysroot、Homebrew 屏蔽等细节

set -e  # 任何命令失败则停止

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${SCRIPT_DIR}/build}"
ARTIFACT_DIR="${ARTIFACT_DIR:-${SCRIPT_DIR}/artifacts}"
# Resolve overrides before entering BUILD_DIR; relative paths otherwise nest on
# each subsequent configure/build invocation.
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="${SCRIPT_DIR}/${BUILD_DIR}" ;; esac
case "$ARTIFACT_DIR" in /*) ;; *) ARTIFACT_DIR="${SCRIPT_DIR}/${ARTIFACT_DIR}" ;; esac
EXTRA_CMAKE_ARGS_STR="${EXTRA_CMAKE_ARGS:-}"
GENERATOR="${CMAKE_GENERATOR:-}"
USE_BOOST_LEGACY_FIND="${USE_BOOST_LEGACY_FIND:-ON}"
PROTORELAY_LOG_BACKEND="${PROTORELAY_LOG_BACKEND:-SPDLOG}"
PROTORELAY_WORKER_BACKEND="${PROTORELAY_WORKER_BACKEND:-STD}"

# 交叉编译 sysroot 路径（家目录下，重启不消失）
CROSS_SYSROOT="${CROSS_SYSROOT:-${HOME}/.protorelay/sysroot/usr}"
CROSS_SYSROOT_SERVER="${CROSS_SYSROOT_SERVER:-root@<SERVER_IP>}"

# 颜色输出
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

print_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

# ---- 仓库拓扑：hub（主检出）还是 agent worktree ----
# 多 agent 并行方案（tools/agents/）里每个 agent 在独立 `git worktree` 中工作，
# 跨 worktree 共享的运行态（构建锁、构建历史、STOP 哨兵）统一放主仓库 .git/agents/。
# 判定：linked worktree 的 --git-dir 是 <hub>/.git/worktrees/<name>，与 --git-common-dir
# （<hub>/.git）不同；主检出两者相同。不在 git 检出内（如 Docker 构建上下文）时全部为空。
_git_abs() {
    local p
    p="$(git -C "$SCRIPT_DIR" rev-parse --path-format=absolute "$1" 2>/dev/null || true)"
    if [ -z "$p" ]; then
        p="$(git -C "$SCRIPT_DIR" rev-parse "$1" 2>/dev/null || true)"
        if [ -n "$p" ]; then
            p="$(cd "$SCRIPT_DIR" && cd "$p" 2>/dev/null && pwd || true)"
        fi
    fi
    printf '%s' "$p"
}
REPO_TOPLEVEL="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel 2>/dev/null || true)"
GIT_DIR_ABS="$(_git_abs --git-dir)"
GIT_COMMON_DIR_ABS="$(_git_abs --git-common-dir)"
IN_AGENT_WORKTREE=0
if [ -n "$GIT_DIR_ABS" ] && [ -n "$GIT_COMMON_DIR_ABS" ] && [ "$GIT_DIR_ABS" != "$GIT_COMMON_DIR_ABS" ]; then
    IN_AGENT_WORKTREE=1
fi
AGENTS_STATE_DIR=""
if [ -n "$GIT_COMMON_DIR_ABS" ]; then
    AGENTS_STATE_DIR="${GIT_COMMON_DIR_ABS}/agents"
    HUB_DIR="$(dirname "$GIT_COMMON_DIR_ABS")"
fi

# ---- 全局构建锁（hub 与所有 worktree 共用一把）----
# 同一台机器同一时刻只跑一个编译任务：本项目不小、机器上可能还有别的项目在跑，
# 串行编译几乎不浪费时间，却能避免多个 agent 同时 -j 编译把内存打爆。
# mkdir 原子抢锁；锁目录内记录 pid / worktree / 起始时间；持有者进程已退出且锁龄
# 超过 60s 视为陈旧自动清理。无人争用时即时获取、脚本退出即释放，单人使用行为不变。
#   BUILD_LOCK_WAIT=<秒>   最长等待（默认 3600），超时退出码 75
#   BUILD_LOCK_DISABLE=1   跳过加锁（仅调试用）
BUILD_LOCK_WAIT="${BUILD_LOCK_WAIT:-3600}"
BUILD_LOCK_DIR=""
BUILD_LOCK_HELD=0
BUILD_STARTED_AT=""
if [ -n "$AGENTS_STATE_DIR" ]; then
    BUILD_LOCK_DIR="${AGENTS_STATE_DIR}/build.lock"
fi

_path_mtime() {
    stat -f %m "$1" 2>/dev/null || stat -c %Y "$1" 2>/dev/null || date +%s
}

acquire_build_lock() {
    if [ "${BUILD_LOCK_DISABLE:-0}" = "1" ]; then
        print_warning "BUILD_LOCK_DISABLE=1: skipping the global build lock"
        return 0
    fi
    if [ -z "$BUILD_LOCK_DIR" ] || ! mkdir -p "$AGENTS_STATE_DIR" 2>/dev/null; then
        print_warning "Not inside a writable git checkout; building without the global build lock"
        return 0
    fi
    local waited=0 holder_pid holder_wt age
    until mkdir "$BUILD_LOCK_DIR" 2>/dev/null; do
        holder_pid="$(cat "${BUILD_LOCK_DIR}/pid" 2>/dev/null || true)"
        holder_wt="$(cat "${BUILD_LOCK_DIR}/worktree" 2>/dev/null || true)"
        age=$(( $(date +%s) - $(_path_mtime "$BUILD_LOCK_DIR") ))
        if [ "$age" -gt 60 ] && { [ -z "$holder_pid" ] || ! kill -0 "$holder_pid" 2>/dev/null; }; then
            print_warning "Removing stale build lock (holder pid ${holder_pid:-?} is gone, age ${age}s)"
            rm -rf "$BUILD_LOCK_DIR"
            continue
        fi
        if [ "$waited" -ge "$BUILD_LOCK_WAIT" ]; then
            print_warning "Timed out after ${waited}s waiting for the global build lock"
            echo "  Holder: pid ${holder_pid:-?} in ${holder_wt:-?}"
            echo "  Lock:   ${BUILD_LOCK_DIR} (removed automatically 60s after the holder exits)"
            exit 75
        fi
        if [ $((waited % 30)) -eq 0 ]; then
            print_info "Waiting for the global build lock: held by pid ${holder_pid:-?} in ${holder_wt:-?} (${waited}s)..."
        fi
        sleep 5
        waited=$((waited + 5))
    done
    echo "$$" > "${BUILD_LOCK_DIR}/pid"
    echo "${REPO_TOPLEVEL:-$SCRIPT_DIR}" > "${BUILD_LOCK_DIR}/worktree"
    echo "${BUILD_ARGS_SUMMARY:-}" > "${BUILD_LOCK_DIR}/cmd"
    date +%s > "${BUILD_LOCK_DIR}/since"
    BUILD_LOCK_HELD=1
    BUILD_STARTED_AT="$(date +%s)"
}

release_build_lock() {
    [ "$BUILD_LOCK_HELD" = "1" ] || return 0
    if [ "$(cat "${BUILD_LOCK_DIR}/pid" 2>/dev/null)" = "$$" ]; then
        rm -rf "$BUILD_LOCK_DIR"
    fi
    BUILD_LOCK_HELD=0
}

# 一行构建历史（时间 / 检出 / 类型 / 退出码 / 耗时），tools/agents/status.sh 用来显示最近构建；
# worktree 里顺带把构建后的真实磁盘占用记回设备账本。
_record_build_history() {
    [ -n "$AGENTS_STATE_DIR" ] && [ -n "$BUILD_STARTED_AT" ] || return 0
    local dur=$(( $(date +%s) - BUILD_STARTED_AT ))
    printf '%s\t%s\t%s\trc=%s\t%ss\n' "$(date '+%Y-%m-%d %H:%M:%S')" "${REPO_TOPLEVEL:-$SCRIPT_DIR}" \
        "${BUILD_ARGS_SUMMARY:-?}" "$1" "$dur" >> "${AGENTS_STATE_DIR}/build-history.tsv" 2>/dev/null || true
    if [ "$IN_AGENT_WORKTREE" = "1" ] && [ -n "$REPO_TOPLEVEL" ] && [ -x "${HUB_DIR}/tools/agents/budget.sh" ]; then
        bash "${HUB_DIR}/tools/agents/budget.sh" record "$REPO_TOPLEVEL" \
            "$(du -sk "$REPO_TOPLEVEL" 2>/dev/null | awk '{print $1}')" >/dev/null 2>&1 || true
    fi
}

# 统一的 EXIT 处理：cross-x64 若临时挪走了 Homebrew 目录先还原，再记历史、放锁。
# 脚本内不要再单独 `trap ... EXIT` 覆盖它。
_build_exit_handler() {
    local rc=$?
    if declare -F _restore_masks >/dev/null 2>&1; then
        _restore_masks
    fi
    _record_build_history "$rc"
    release_build_lock
    exit "$rc"
}

cleanup_root_cmake_artifacts() {
    local root_artifacts=(
        "${SCRIPT_DIR}/CMakeCache.txt"
        "${SCRIPT_DIR}/cmake_install.cmake"
        "${SCRIPT_DIR}/Makefile"
        "${SCRIPT_DIR}/CMakeFiles"
    )

    local found=0
    for path in "${root_artifacts[@]}"; do
        if [ -e "$path" ]; then
            found=1
            break
        fi
    done

    if [ "$found" -eq 1 ]; then
        print_warning "Found in-source CMake generated files at project root; cleaning them..."
        rm -rf "${root_artifacts[@]}"
        print_info "Root CMake artifacts cleaned. Build outputs will stay under: ${BUILD_DIR}"
    fi
}

# ---- pure-log 模式 ----
# ./build.sh --pure-log Release → 变换 LOG_* 为 LOG_PURE(hash, args, ts)，然后构建
PURE_LOG_DIR="${PURE_LOG_DIR:-${SCRIPT_DIR}/build/transformed}"
if [[ "$1" == "--pure-log" ]]; then
    shift
    if [ "$#" -lt 1 ]; then
        echo "Usage: $0 --pure-log <Debug|Release|SafeRelease> [...]"
        exit 1
    fi
    echo -e "${BLUE}[PURE-LOG]${NC} Transforming LOG_* → LOG_PURE ..."
    python3 "${SCRIPT_DIR}/tools/log_transform.py" "${SCRIPT_DIR}" --out "$PURE_LOG_DIR"
    echo -e "${BLUE}[PURE-LOG]${NC} Building from transformed sources: $PURE_LOG_DIR"
    cd "$PURE_LOG_DIR"
    exec bash build.sh "$@"
fi

# 使用方法
if [ "$#" -lt 1 ]; then
    echo "Usage: $0 [--pure-log] <Debug|Release|SafeRelease|Asan> [clean] [jobs] [object-only] [cross-x64] [no-tests]"
    echo "       $0 sync-sysroot [server]"
    echo ""
    echo "Note: build 永远 per-target 串行（lib → exe → test）+ 单 target 内并行（默认 -j 4）"
    echo "      防止 16GB 机器上 30+ target 全并行 OOM；数字参数 <jobs> = per-target 并行度"
    echo ""
    echo "Examples:"
    echo "  $0 Debug           # 构建 Debug 版本（无优化，启用所有调试日志）"
    echo "  $0 Release         # 构建 Release 版本（高优化，仅 INFO 级别日志）"
    echo "  $0 --pure-log Release  # 日志压缩模式：LOG_* → LOG_PURE(hash, args, ts)"
    echo "  $0 SafeRelease     # 低内存兜底构建（2核2G服务器推荐，-j 1/target）"
    echo "  $0 Asan cross-x64  # ASan+UBSan 诊断构建（线上抓堆损坏用，配 deploy.sh BUILD_TYPE=Asan）"
    echo "  $0 Debug clean     # 清理后重新构建 Debug 版本"
    echo "  $0 Release clean   # 清理后重新构建 Release 版本"
    echo "  $0 Release 1       # per-target -j 1（极低内存服务器）"
    echo "  $0 Release 8       # per-target -j 8（32GB+ 推荐）"
    echo "  $0 Release clean 1 object-only   # 仅编译 .o，不做最终链接"
    echo "  $0 Debug object-only             # 快速验证编译（需头文件，不强制链接库）"
    echo "  $0 Release cross-x64             # 交叉编译到 Linux x86_64（自动处理 sysroot）"
    echo "  $0 Release no-tests              # 跳过 test/bench 目标（重构时有用）"
    echo "  $0 sync-sysroot                  # 从服务器同步 spdlog/fmt 头文件到本地 sysroot"
    echo ""
    echo "Env override: BUILD_JOBS=<n>, EXTRA_CMAKE_ARGS='<...>'"
    echo "              ARTIFACT_DIR=<path>, CROSS_CC=<path>, CROSS_CXX=<path>"
    echo "              USE_BOOST_LEGACY_FIND=<ON|OFF>"
    echo "              PROTORELAY_LOG_BACKEND=<SPDLOG|STANDARD|NONE>"
    echo "              PROTORELAY_WORKER_BACKEND=<STD|BOOST>"
    echo "              CROSS_SYSROOT=<path>, CROSS_SYSROOT_SERVER=<ssh-host>"
    echo "              SERIAL_JOBS=<n>  # per-target 并行度（默认 4，可被数字参数覆盖）"
    echo "              PURE_LOG_DIR=<path>  # 纯日志变换输出目录"
    echo "              USE_CCACHE=<auto|1|0> # auto: 新构建目录自动用 ccache，老目录不碰；1 强制开；0 关"
    echo "              BUILD_LOCK_WAIT=<秒> # 全局构建锁最长等待（默认 3600；hub+所有 worktree 同时只跑一个构建）"
    echo "              BUILD_BUDGET_HEADROOM_GB=<n> # worktree 构建前按设备磁盘账本预留的增量（默认 4；Debug 建议 20）"
    echo ""
    echo "Multi-agent: 每个 agent 在独立 git worktree 里工作（tools/agents/，规则见 AGENTS.md）；"
    echo "             worktree 内禁止 cross-x64 / object-only / sync-sysroot（生产部署链）。"
    echo ""
    exit 1
fi

# ---- sync-sysroot 子命令 ----
if [ "$1" = "sync-sysroot" ]; then
    if [ "$IN_AGENT_WORKTREE" = "1" ]; then
        print_warning "Refusing 'sync-sysroot' from an agent worktree: it talks to the production server"
        echo "  Multi-agent sessions must never touch production (see AGENTS.md). Run this from the hub checkout."
        exit 77
    fi
    SERVER="${2:-${CROSS_SYSROOT_SERVER}}"
    SYSROOT="$CROSS_SYSROOT"
    echo -e "${BLUE}[INFO]${NC} Syncing sysroot headers from ${SERVER}..."
    echo -e "${BLUE}[INFO]${NC} Target: ${SYSROOT}"
    mkdir -p "$SYSROOT"
    ssh "$SERVER" "tar czf - -C /usr/include spdlog fmt" | tar xzf - -C "$SYSROOT"
    # SYSROOT_BOOST=1：同时同步 boost 头文件（与目标机发行版完全一致）。
    # 背景：brew boost(1.90/asio 1.36) 头文件 + GCC13 交叉编译，resolve→async_connect
    # 路径在 linux 上必崩（2026-09-11 RackNerd 实测，最小复现 30 行即崩）；docker e2e
    # 用的是 Ubuntu 自带 1.74 头文件所以从没暴露。目标是 1.74 运行库时应同步。
    if [[ "$SYSROOT_BOOST" == "1" ]]; then
        echo -e "${BLUE}[INFO]${NC} Syncing boost headers (SYSROOT_BOOST=1)..."
        ssh "$SERVER" "tar czf - -C /usr/include boost" | tar xzf - -C "$SYSROOT"
    fi
    FILE_COUNT=$(find "$SYSROOT" -type f 2>/dev/null | wc -l | tr -d ' ')
    print_success "Sysroot synced: ${FILE_COUNT} files at ${SYSROOT}"
    exit 0
fi

BUILD_TYPE="$1"
BUILD_ARGS_SUMMARY="$*"
CLEAN_BUILD=""
USER_JOBS=""
BUILD_OBJECT_ONLY="OFF"
CROSS_X64_LINUX="OFF"
BUILD_TESTS="ON"
CROSS_CC="${CROSS_CC:-}"
CROSS_CXX="${CROSS_CXX:-}"

# 解析可选参数，顺序不限：clean / <jobs> / object-only / cross-x64
# 注：build 永远是 per-target 串行（防 16GB OOM），无 opt-out flag；
# 数字参数 <jobs> 复用为 per-target 并行度（旧语义 = 全局 -j 已废弃）
for arg in "${@:2}"; do
    if [[ "$arg" == "clean" ]]; then
        CLEAN_BUILD="clean"
    elif [[ "$arg" =~ ^[0-9]+$ ]]; then
        USER_JOBS="$arg"
    elif [[ "$arg" == "object-only" || "$arg" == "obj-only" || "$arg" == "nolink" ]]; then
        BUILD_OBJECT_ONLY="ON"
    elif [[ "$arg" == "cross-x64" || "$arg" == "cross-linux-x64" || "$arg" == "cross-ubuntu24" || "$arg" == "x64-ubuntu24" ]]; then
        CROSS_X64_LINUX="ON"
    elif [[ "$arg" == "no-tests" || "$arg" == "no-test" || "$arg" == "skip-tests" ]]; then
        BUILD_TESTS="OFF"
    else
        print_warning "Unknown argument: $arg"
        echo "Allowed optional args: clean, <jobs>, object-only, cross-x64, no-tests"
        exit 1
    fi
done

# ---- agent worktree 红线：不碰生产 ----
# cross-x64 / object-only 是对象部署链（产物给线上用，还会临时挪动 Homebrew 目录），
# 多 agent 并行会话一律不允许；只有 hub 主检出里的人可以跑。
# .git/agents/STOP 哨兵存在时，worktree 里的新构建也一律拒绝（给人一个紧急刹车）。
if [ "$IN_AGENT_WORKTREE" = "1" ]; then
    if [ "$CROSS_X64_LINUX" = "ON" ] || [ "$BUILD_OBJECT_ONLY" = "ON" ]; then
        print_warning "Refusing '${BUILD_ARGS_SUMMARY}' from an agent worktree: cross-x64 / object-only are production deployment paths"
        echo "  Worktree: ${REPO_TOPLEVEL}"
        echo "  Multi-agent sessions must never touch production (see AGENTS.md); run such builds from the hub only."
        exit 77
    fi
    if [ -n "$AGENTS_STATE_DIR" ] && [ -e "${AGENTS_STATE_DIR}/STOP" ]; then
        print_warning "STOP sentinel present (${AGENTS_STATE_DIR}/STOP): agent builds are paused by the operator"
        exit 76
    fi
fi

# 不同编译模式使用独立的构建目录，避免 host/cross 切换时互相清缓存
if [[ "$CROSS_X64_LINUX" == "ON" ]]; then
    BUILD_DIR="${BUILD_DIR}/cross-x64"
fi

resolve_cross_compiler() {
    local env_cc="$1" env_cxx="$2" cc cxx

    if [ -n "$env_cc" ] && [ -n "$env_cxx" ]; then
        cc="$env_cc"
        cxx="$env_cxx"
    else
        cc="$(command -v x86_64-linux-gnu-gcc || true)"
        cxx="$(command -v x86_64-linux-gnu-g++ || true)"
    fi

    if [ -z "$cc" ] || [ -z "$cxx" ]; then
        print_warning "cross-x64 mode requested but x86_64-linux-gnu toolchain not found"
        echo "Install toolchain or set CROSS_CC/CROSS_CXX explicitly."
        exit 1
    fi

    CROSS_CC="$cc"
    CROSS_CXX="$cxx"
}

# 验证构建类型
if [[ "$BUILD_TYPE" != "Debug" && "$BUILD_TYPE" != "Release" && "$BUILD_TYPE" != "SafeRelease" && "$BUILD_TYPE" != "Asan" ]]; then
    print_warning "Invalid build type: $BUILD_TYPE"
    echo "Must be 'Debug', 'Release', 'SafeRelease' or 'Asan'"
    exit 1
fi

# ---- 交叉编译预检：工具链 + sysroot（必须在清理构建目录之前） ----
if [[ "$CROSS_X64_LINUX" == "ON" ]]; then
    # 1. 检查交叉编译器
    resolve_cross_compiler "$CROSS_CC" "$CROSS_CXX"
    print_info "Cross C compiler : ${CROSS_CC}"
    print_info "Cross C++ compiler: ${CROSS_CXX}"

    # 2. 检查 sysroot 头文件（服务器 spdlog/fmt）
    if [[ "$PROTORELAY_LOG_BACKEND" == "SPDLOG" ]] && { [ ! -d "$CROSS_SYSROOT/spdlog" ] || [ ! -d "$CROSS_SYSROOT/fmt" ]; }; then
        print_warning "Sysroot headers not found at: $CROSS_SYSROOT"
        echo ""
        echo "  The cross-x64 build needs server-side spdlog/fmt headers to avoid ABI mismatches."
        echo "  Run once to sync them (they will persist across reboots):"
        echo ""
        echo -e "    ${GREEN}$0 sync-sysroot${NC}"
        echo ""
        echo "  Or set CROSS_SYSROOT env to an alternative path."
        exit 1
    fi
    print_info "Sysroot headers : ${CROSS_SYSROOT}"

    # 3. 交叉编译强制 object-only（避免 host/target link 不匹配）
    if [[ "$BUILD_OBJECT_ONLY" != "ON" ]]; then
        print_warning "cross-x64 defaults to object-only to avoid host/target link mismatch"
        BUILD_OBJECT_ONLY="ON"
    fi
fi

CMAKE_BUILD_TYPE="$BUILD_TYPE"
SAFE_CMAKE_ARGS=()

# SafeRelease: low-memory fallback profile for tiny servers.
if [[ "$BUILD_TYPE" == "SafeRelease" ]]; then
    CMAKE_BUILD_TYPE="Release"
    SAFE_CMAKE_ARGS+=("-DENABLE_DEBUG_LOGS=OFF")
    SAFE_CMAKE_ARGS+=("-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG -mtune=generic")
    SAFE_CMAKE_ARGS+=("-DCMAKE_C_FLAGS_RELEASE=-O2 -DNDEBUG -mtune=generic")
fi

# Asan: 线上内存问题诊断构建（ASan+UBSan 插桩，-O1 保留栈帧）。
# 编译侧 flag 由 ENABLE_ASAN_SERVER 挂到 *_obj 目标；链接侧 -fsanitize
# 由 link.sh 的 LINK_EXTRA_FLAGS 提供（deploy.sh 在 BUILD_TYPE=Asan 时自动带上）。
# 调试级别由 ASAN_GLEVEL 控制（默认 1=行号表）：
#   ASAN_GLEVEL=1  → -g1，ASan 报告可直接还原 文件:行号；
#                     但小内存服务器（2G）上 ld 峰值 ~900MB，曾把机器顶进
#                     swap 风暴（2026-09-05 实测 load 42、sshd/邮件端口全部
#                     无响应）。用前确认服务器内存充裕或已换 gold 链接。
#   ASAN_GLEVEL=0  → 无调试信息，链接内存≈release 水平（保底方案）；
#                     报告只有函数名栈，行号用本地 -g1 对象 addr2line 补
#                     （-g 级别不影响代码布局，两边地址一致）。
# 注意：ASan 换掉了分配器，glibc 的 tcache/堆元数据检查不再触发——
# 同一个 bug 在 Asan 版下会以 heap-use-after-free / heap-buffer-overflow
# 带完整分配+释放双栈的形式在第一现场报警。
ASAN_GLEVEL="${ASAN_GLEVEL:-1}"
if [[ "$BUILD_TYPE" == "Asan" ]]; then
    CMAKE_BUILD_TYPE="Release"
    SAFE_CMAKE_ARGS+=("-DENABLE_ASAN_SERVER=ON")
    SAFE_CMAKE_ARGS+=("-DCMAKE_CXX_FLAGS_RELEASE=-O1 -g${ASAN_GLEVEL} -DNDEBUG -fno-omit-frame-pointer")
    SAFE_CMAKE_ARGS+=("-DCMAKE_C_FLAGS_RELEASE=-O1 -g${ASAN_GLEVEL} -DNDEBUG -fno-omit-frame-pointer")
fi

# ---- agent worktree 磁盘预算（设备级，生产修复 #1）：余量不足就不开编译 ----
# check 会把本次构建的预计增量（默认 4G；Debug 建议 BUILD_BUDGET_HEADROOM_GB=20）算进全设备账本。
if [ "$IN_AGENT_WORKTREE" = "1" ] && [ -x "${HUB_DIR}/tools/agents/budget.sh" ]; then
    _budget_rc=0
    _budget_msg="$(bash "${HUB_DIR}/tools/agents/budget.sh" check "${BUILD_BUDGET_HEADROOM_GB:-4}")" || _budget_rc=$?
    if [ "$_budget_rc" != "0" ]; then
        printf '%s\n' "$_budget_msg" >&2
        print_warning "Refusing to build from this worktree: device agent-disk budget check failed (exit ${_budget_rc})"
        exit 78
    fi
    print_info "$_budget_msg"
    unset _budget_rc _budget_msg
fi

# ---- 从这里开始会改动构建目录 / 占用编译资源：先拿全局构建锁 ----
trap _build_exit_handler EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
acquire_build_lock

# 清理构建目录（可选）
if [ "$CLEAN_BUILD" = "clean" ]; then
    print_info "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
fi

# 创建构建目录
cleanup_root_cmake_artifacts
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# 自动修复：如果 build 目录里的 CMakeCache 来自其它源码目录，清理后重新配置。
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
    CACHED_SOURCE_DIR="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$BUILD_DIR/CMakeCache.txt" | head -n1)"
    if [ -n "$CACHED_SOURCE_DIR" ] && [ "$CACHED_SOURCE_DIR" != "$SCRIPT_DIR" ]; then
        print_warning "Detected stale CMake cache from: $CACHED_SOURCE_DIR"
        print_info "Auto-cleaning build directory due to source mismatch..."
        rm -rf "$BUILD_DIR"
        mkdir -p "$BUILD_DIR"
        cd "$BUILD_DIR"
    fi
fi

detect_cpu_count() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
        return
    fi
    if command -v sysctl >/dev/null 2>&1; then
        sysctl -n hw.ncpu
        return
    fi
    echo 1
}

detect_mem_kb() {
    if [ -r /proc/meminfo ]; then
        awk '/MemAvailable:/ {print $2; exit}' /proc/meminfo
        return
    fi
    if command -v sysctl >/dev/null 2>&1; then
        # macOS fallback
        local bytes
        bytes=$(sysctl -n hw.memsize 2>/dev/null || echo 0)
        if [ "$bytes" -gt 0 ] 2>/dev/null; then
            echo $((bytes / 1024))
            return
        fi
    fi
    echo 0
}

calculate_default_jobs() {
    local cpu mem_kb jobs_by_mem jobs
    cpu=$(detect_cpu_count)
    mem_kb=$(detect_mem_kb)

    # Conservative estimate: ~1.2GB available memory per compile job.
    if [ "$mem_kb" -gt 0 ] 2>/dev/null; then
        jobs_by_mem=$((mem_kb / 1200000))
    else
        jobs_by_mem=1
    fi

    if [ "$jobs_by_mem" -lt 1 ]; then
        jobs_by_mem=1
    fi

    if [ "$cpu" -lt "$jobs_by_mem" ]; then
        jobs=$cpu
    else
        jobs=$jobs_by_mem
    fi

    if [ "$jobs" -lt 1 ]; then
        jobs=1
    fi

    echo "$jobs"
}

if [ -n "${BUILD_JOBS:-}" ]; then
    BUILD_JOBS_VALUE="$BUILD_JOBS"
elif [ -n "$USER_JOBS" ]; then
    BUILD_JOBS_VALUE="$USER_JOBS"
else
    BUILD_JOBS_VALUE="$(calculate_default_jobs)"
fi

if [[ "$BUILD_TYPE" == "SafeRelease" && -z "${BUILD_JOBS:-}" && -z "$USER_JOBS" ]]; then
    # Hard-cap fallback profile to 1 job on constrained instances.
    BUILD_JOBS_VALUE=1
fi

if ! [[ "$BUILD_JOBS_VALUE" =~ ^[0-9]+$ ]] || [ "$BUILD_JOBS_VALUE" -lt 1 ]; then
    print_warning "Invalid jobs value: $BUILD_JOBS_VALUE"
    echo "jobs must be an integer >= 1"
    exit 1
fi

# CMake 配置
print_info "Configuring CMake for ${BUILD_TYPE} build..."
cmake_args=(
    -S "$SCRIPT_DIR"
    -B "$BUILD_DIR"
    -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE"
    -DBUILD_OBJECT_ONLY="$BUILD_OBJECT_ONLY"
    -DBUILD_TESTS="$BUILD_TESTS"
    -DPROTORELAY_LOG_BACKEND="$PROTORELAY_LOG_BACKEND"
    -DPROTORELAY_WORKER_BACKEND="$PROTORELAY_WORKER_BACKEND"
)

if [[ "$USE_BOOST_LEGACY_FIND" == "ON" ]]; then
    cmake_args+=( -DBoost_NO_BOOST_CMAKE=ON )
fi

if [[ "$CROSS_X64_LINUX" == "ON" ]]; then
    _CROSS_COMPILER_SYSROOT="$($CROSS_CXX -print-sysroot 2>/dev/null || true)"
    cmake_args+=(
        -DCMAKE_SYSTEM_NAME=Linux
        -DCMAKE_SYSTEM_PROCESSOR=x86_64
        -DCMAKE_CXX_COMPILER="$CROSS_CXX"
        -DSYSROOT_INCLUDE="$CROSS_SYSROOT"
    )

    if [ -n "$_CROSS_COMPILER_SYSROOT" ]; then
        cmake_args+=( -DCMAKE_SYSROOT="$_CROSS_COMPILER_SYSROOT" )
    fi

    # ---- 自动屏蔽 Homebrew spdlog/fmt（防止 cmake 误找） ----
    _HB_SPDLOG_CMAKE="/opt/homebrew/lib/cmake/spdlog"
    _HB_SPDLOG_INC="/opt/homebrew/include/spdlog"
    _HB_FMT_INC="/opt/homebrew/include/fmt"
    _MASKS=()
    for _d in "$_HB_SPDLOG_CMAKE" "$_HB_SPDLOG_INC" "$_HB_FMT_INC"; do
        [[ "$PROTORELAY_LOG_BACKEND" == "SPDLOG" ]] || continue
        if [ -d "$_d" ] || [ -f "$_d" ]; then
            _MASKS+=("$_d")
        fi
    done
    if [ ${#_MASKS[@]} -gt 0 ]; then
        print_info "Temporarily masking Homebrew spdlog/fmt for cmake configure..."
        for _d in "${_MASKS[@]}"; do
            mv "$_d" "${_d}.bak"
        done
        # 异常退出时由统一的 _build_exit_handler（EXIT trap）调用 _restore_masks 还原；
        # 这里不要再 trap EXIT，否则会覆盖掉锁释放。
        _restore_masks() {
            for _d in "${_MASKS[@]}"; do
                if [ -e "${_d}.bak" ] && [ ! -e "$_d" ]; then
                    mv "${_d}.bak" "$_d"
                fi
            done
        }
    fi

    # Cross mode commonly lacks target libcurl development files on macOS hosts.
    # Default to OFF unless user explicitly overrides via EXTRA_CMAKE_ARGS.
    if [[ "$EXTRA_CMAKE_ARGS_STR" != *"ENABLE_HDFS_WEB_STORAGE="* ]]; then
        print_info "cross-x64 mode: forcing -DENABLE_HDFS_WEB_STORAGE=OFF (override via EXTRA_CMAKE_ARGS if needed)"
        cmake_args+=( -DENABLE_HDFS_WEB_STORAGE=OFF )
    fi
    if [[ "$EXTRA_CMAKE_ARGS_STR" != *"ENABLE_S3_STORAGE="* ]]; then
        print_info "cross-x64 mode: forcing -DENABLE_S3_STORAGE=OFF (override via EXTRA_CMAKE_ARGS if needed)"
        cmake_args+=( -DENABLE_S3_STORAGE=OFF )
    fi

    # Cross artifacts are usually used for production deployment; keep debug logs OFF
    # unless the user explicitly opts in.
    if [[ "$EXTRA_CMAKE_ARGS_STR" != *"ENABLE_DEBUG_LOGS="* ]]; then
        print_info "cross-x64 mode: forcing -DENABLE_DEBUG_LOGS=OFF (override via EXTRA_CMAKE_ARGS if needed)"
        cmake_args+=( -DENABLE_DEBUG_LOGS=OFF )
    fi

    # cross-x64: override -march=native (invalid for cross-compiler on macOS)
    # 默认 x86-64-v3（AVX2 一代，阿里云 CPU 支持）。目标机更老时用 CROSS_ARCH 覆盖，
    # 如 RackNerd E5-2680 v2（Ivy Bridge）只到 v2：
    #   CROSS_ARCH=x86-64-v2 bash build.sh Release clean cross-x64
    cmake_args+=(
        -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -march=${CROSS_ARCH:-x86-64-v3}"
        -DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG -march=${CROSS_ARCH:-x86-64-v3}"
    )
fi

if [ -n "$GENERATOR" ]; then
    cmake_args+=( -G "$GENERATOR" )
fi

# ---- ccache：hub 与所有 worktree 共享编译缓存，新 worktree 的冷构建近乎免费 ----
# USE_CCACHE=auto（默认）：新建的构建目录、以及已经在用 ccache 的目录自动启用；
#                          已有但没用过 ccache 的老目录不碰（否则会触发一次全量重编），只提示。
# USE_CCACHE=1：对当前目录强制启用（接受一次全量重编）。USE_CCACHE=0：关闭并撤掉 launcher。
# EXTRA_CMAKE_ARGS 里已显式给了 *_COMPILER_LAUNCHER 时不干预。
USE_CCACHE="${USE_CCACHE:-auto}"
if [[ "$EXTRA_CMAKE_ARGS_STR" != *"COMPILER_LAUNCHER"* ]]; then
    if [ "$USE_CCACHE" = "0" ]; then
        cmake_args+=( -UCMAKE_C_COMPILER_LAUNCHER -UCMAKE_CXX_COMPILER_LAUNCHER )
    elif command -v ccache >/dev/null 2>&1; then
        _ccache_fresh_dir=1
        _ccache_already_on=0
        if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
            _ccache_fresh_dir=0
            if grep -q '^CMAKE_CXX_COMPILER_LAUNCHER:.*=ccache$' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null; then
                _ccache_already_on=1
            fi
        fi
        if [ "$USE_CCACHE" = "1" ] || [ "$_ccache_fresh_dir" = "1" ] || [ "$_ccache_already_on" = "1" ]; then
            cmake_args+=( -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache )
        else
            print_info "ccache is installed but left off for this existing build dir (enabling = one-time full rebuild); opt in with USE_CCACHE=1"
        fi
    fi
fi

if [ -n "$EXTRA_CMAKE_ARGS_STR" ]; then
    # shellcheck disable=SC2206
    user_extra_args=( $EXTRA_CMAKE_ARGS_STR )
    cmake_args+=( "${user_extra_args[@]}" )
fi

cmake_args+=( "${SAFE_CMAKE_ARGS[@]}" )

cmake "${cmake_args[@]}"

# ---- 恢复 Homebrew（cmake configure 完成后即可恢复） ----
if [ ${#_MASKS[@]} -gt 0 ]; then
    for _d in "${_MASKS[@]}"; do
        if [ -e "${_d}.bak" ] && [ ! -e "$_d" ]; then
            mv "${_d}.bak" "$_d"
        fi
    done
    unset _MASKS _restore_masks _HB_SPDLOG_CMAKE _HB_SPDLOG_INC _HB_FMT_INC
fi

# 编译
# ---- per-target 串行 + 单 target 内并行（强制 always-on 防 OOM） ----
# 之前默认 -j 12 在 16GB 机器上 30+ target 同时抢内存会 OOM+swap 风暴
# （vm_stat 累计 9 千万+ swapin / 1亿+ swapout，load avg 飙到 77）。
# 改为：target 串行（mailServer lib → 各 exe → 各 test），单 target 内
# 默认 -j 4（4 路编译各 ~1.5GB + 链接器 ~2GB = 峰值 ~8GB，远低于 16GB）。
# 调高方法：`bash build.sh Release 8`（= 8 jobs/target）或 env SERIAL_JOBS=8
# （适合 32GB+）。
if [ -n "${SERIAL_JOBS:-}" ]; then
    SERIAL_PER_TARGET_JOBS="$SERIAL_JOBS"
elif [ -n "$USER_JOBS" ]; then
    # 数字参数复用为 per-target 并行度（避免 -j 全并行的危险行为）
    SERIAL_PER_TARGET_JOBS="$USER_JOBS"
else
    SERIAL_PER_TARGET_JOBS=4
fi
if [ "$SERIAL_PER_TARGET_JOBS" -gt "$BUILD_JOBS_VALUE" ]; then
    SERIAL_PER_TARGET_JOBS="$BUILD_JOBS_VALUE"
fi
print_info "Building per-target: ${SERIAL_PER_TARGET_JOBS} jobs/target (target order: lib → exe → test)"

# 目标顺序：lib 先（最重并行工作），再 exe（依赖 lib + 链接器峰值），
# 最后 test（依赖 lib）
# ⚠ object-only 模式下 CMake 定义的是 *_obj 对象库 target（smtpsServer_obj 等），
# 没有 "mailServer" 这个 exe target —— 之前这里写死 mailServer 导致
# `make: No rule to make target` 直接失败。2026-08-29 修复。
if [ "$BUILD_OBJECT_ONLY" = "ON" ]; then
    SERIAL_TARGETS_LIB=("smtpsServer_obj" "imapsServer_obj" "mailServer_obj" "webServer_obj")
else
    SERIAL_TARGETS_LIB=("mailServer")
fi
SERIAL_TARGETS_EXE=("smtpsServer" "imapsServer" "pop3Server" "webmailServer" "h2web" "smtp_client" "webServer")

for target in "${SERIAL_TARGETS_LIB[@]}" "${SERIAL_TARGETS_EXE[@]}"; do
    # ⚠ object-only 模式只跳过 exe target（CMake 只定义了 *_obj 对象库，
    # exe target 不存在，`make: No rule to make target` 直接失败）。
    # 注意 2026-08-29 的"修复"把条件写反了：默认 skip=1 + 命中 exe 才 skip=0，
    # 结果跳过全部 *_obj 库、反去编 exe → 一个 .o 都不编还直接报错。
    # 正确语义：默认构建（skip=0），命中 exe 才跳过。2026-09-01 修正。
    if [ "$BUILD_OBJECT_ONLY" = "ON" ]; then
        skip=0
        for _exe in "${SERIAL_TARGETS_EXE[@]}"; do
            [[ "$target" == "$_exe" ]] && { skip=1; break; }
        done
        [[ $skip -eq 1 ]] && continue
    fi
    # 2026-09-06 补：目标清单里可能存在 CMake 已不再定义的旧 target
    # （如 smtp_client/webServer），直接 make 会 No rule 中断整个构建。
    # 与下方 test 循环同款：先探测存在性，不存在就跳过并提示。
    if ! cmake --build "$BUILD_DIR" --target help 2>/dev/null | grep -qE "^\.\.\. ${target}\$"; then
        print_warning "target '$target' not defined by CMake, skipping"
        continue
    fi
    print_info "  → $target (jobs=$SERIAL_PER_TARGET_JOBS)"
    cmake --build "$BUILD_DIR" --target "$target" -j"$SERIAL_PER_TARGET_JOBS"
done

if [ "$BUILD_TESTS" = "ON" ]; then
    # 抓 CMake 已注册的所有 test target（名字以 _test 结尾），排除聚合 unit_tests
    SERIAL_TEST_TARGETS=$(cmake --build "$BUILD_DIR" --target help 2>/dev/null \
        | grep -E "^\.\.\. " | awk '{print $2}' \
        | grep -E "(_test$|^test_inbound_verifier$|^outbound_smoke$)" | grep -vE "^unit_tests$" || true)
    for target in $SERIAL_TEST_TARGETS; do
        print_info "  → $target (jobs=$SERIAL_PER_TARGET_JOBS)"
        cmake --build "$BUILD_DIR" --target "$target" -j"$SERIAL_PER_TARGET_JOBS"
    done
fi

# 输出结果
print_success "Build completed successfully!"
print_info "Build type: $BUILD_TYPE"
if [ "$CROSS_X64_LINUX" = "ON" ]; then
    print_info "Cross target: Linux x86_64 (Ubuntu 24.04 compatible)"
    print_info "Cross C compiler: $CROSS_CC"
    print_info "Cross CXX compiler: $CROSS_CXX"
fi
if [ "$BUILD_OBJECT_ONLY" = "ON" ]; then
    print_info "Mode: object-only (no final executable link)"
    print_info "Objects: ${BUILD_DIR}/CMakeFiles/smtpsServer_obj.dir/..."
else
    print_info "Executable: ${BUILD_DIR}/smtpsServer"
fi

# 导出构建产物到显眼目录，方便 scp 传输。
TARGET_TAG="host"
if [ "$CROSS_X64_LINUX" = "ON" ]; then
    TARGET_TAG="linux-x86_64"
fi

MODE_TAG="bin"
if [ "$BUILD_OBJECT_ONLY" = "ON" ]; then
    MODE_TAG="obj"
fi

EXPORT_DIR="${ARTIFACT_DIR}/${TARGET_TAG}/${BUILD_TYPE}/${MODE_TAG}"
rm -rf "$EXPORT_DIR"
mkdir -p "$EXPORT_DIR"

if [ "$BUILD_OBJECT_ONLY" = "ON" ]; then
    # 导出所有对象库的 .o 文件
    for obj_dir in "$BUILD_DIR"/CMakeFiles/*.dir; do
        [ -d "$obj_dir" ] || continue
        while IFS= read -r obj; do
            # 路径格式: build/CMakeFiles/<target>.dir/src/.../file.cpp.o
            # 去除 build/ 前缀，保留 CMakeFiles/<target>.dir/ 后面的部分
            rel="${obj#${BUILD_DIR}/}"
            out_dir="${EXPORT_DIR}/$(dirname "$rel")"
            mkdir -p "$out_dir"
            cp "$obj" "$out_dir/"
        done < <(find "$obj_dir" -type f -name '*.o' | sort)
    done

    # ---- 剪枝孤儿 .o（2026-10-22 生产启动 SEGV 的根因防御）----
    # CMake 增量构建不会清理"源文件已被删除/搬移"的旧 .o，它们留在构建目录里
    # 并被上面的镜像导出原样带进部署产物；而 link.sh 按 obj 根目录全量收集 .o。
    # 孤儿里冻结的旧版代码一旦与现行代码产生同名符号（重构改类布局/改名后必然），
    # ELF 的"强符号覆盖弱符号"会让旧版 out-of-line 定义劫持现行头文件 inline（弱）
    # 符号：2026-10-22 事故中，删除两个月前的 back/storage/local_file_storage_
    # provider.cpp.o 里旧版构造函数（强）覆盖了现行包装类的 inline 构造函数（弱），
    # 以旧成员布局初始化新双继承对象 → pr:: 基子对象的 directories_ 向量从未构造
    # → bootstrap 阶段虚调用 ensure_ready 解引用垃圾指针，smtps/imaps 双双启动即崩。
    # 判据：导出树里的每个 .o 必须能在当前检出中找到对应源文件，否则删除。
    PRUNED=0
    while IFS= read -r obj; do
        rel="${obj#${EXPORT_DIR}/CMakeFiles/}"
        src="${rel#*.dir/}"
        src="${src%.o}"
        if [ ! -f "${SCRIPT_DIR}/${src}" ]; then
            rm -f "$obj"
            print_warning "Pruned orphan object (source no longer exists): ${src}"
            PRUNED=$((PRUNED + 1))
        fi
    done < <(find "${EXPORT_DIR}/CMakeFiles" -type f -name '*.o' 2>/dev/null | sort)
    if [ "$PRUNED" -gt 0 ]; then
        print_info "Pruned ${PRUNED} orphan object file(s) from export"
    fi

    # 把链接脚本也放进去，目标机可直接复用。
    if [ -f "${SCRIPT_DIR}/link.sh" ]; then
        cp "${SCRIPT_DIR}/link.sh" "${EXPORT_DIR}/"
        chmod +x "${EXPORT_DIR}/link.sh"
    fi

    OBJ_COUNT=$(find "$EXPORT_DIR" -type f -name '*.o' | wc -l | tr -d ' ')
    print_info "Exported object files: ${OBJ_COUNT}"
else
    if [ -f "${BUILD_DIR}/smtpsServer" ]; then
        cp "${BUILD_DIR}/smtpsServer" "${EXPORT_DIR}/"
        chmod +x "${EXPORT_DIR}/smtpsServer"
    fi
    if [ -f "${BUILD_DIR}/webServer" ]; then
        cp "${BUILD_DIR}/webServer" "${EXPORT_DIR}/"
        chmod +x "${EXPORT_DIR}/webServer"
    fi
fi

print_info "Artifact export dir: ${EXPORT_DIR}"
print_info "SCP example: scp -r '${EXPORT_DIR}' user@server:/path/to/deploy/"
print_info ""

# 显示编译配置摘要
if [ "$BUILD_TYPE" = "Debug" ]; then
    echo -e "${GREEN}Debug Mode Enabled:${NC}"
    echo "  • Optimization: -O0 (no optimization)"
    echo "  • Debug symbols: -g (included)"
    echo "  • Debug logs: ALL levels enabled"
    echo "  • Frame pointers: enabled (-fno-omit-frame-pointer)"
    echo ""
    if [ "$BUILD_OBJECT_ONLY" = "ON" ]; then
        echo "  • Link step: skipped (object-only)"
        echo "Object files are ready for target-machine linking."
    else
        echo "Start server with: ./build/smtpsServer"
    fi
elif [ "$BUILD_TYPE" = "Release" ]; then
    echo -e "${GREEN}Release Mode Enabled:${NC}"
    echo "  • Optimization: -O3 (high optimization)"
    echo "  • Native tuning: -march=native"
    echo "  • Debug logs: INFO level only (DEBUG disabled)"
    echo "  • NDEBUG flag: enabled"
    echo ""
    if [ "$BUILD_OBJECT_ONLY" = "ON" ]; then
        echo "  • Link step: skipped (object-only)"
        echo "Object files are ready for target-machine linking."
    else
        echo "Start server with: ./build/smtpsServer"
        echo "Or run tests with: cd test && uv run cl.py"
    fi
elif [ "$BUILD_TYPE" = "Asan" ]; then
    echo -e "${GREEN}Asan Diagnostic Mode Enabled (GLEVEL=${ASAN_GLEVEL}):${NC}"
    echo "  • Sanitizers: AddressSanitizer + UBSan (halt on error)"
    echo "  • Optimization: -O1 (keep frames resolvable)"
    echo "  • Debug: -g${ASAN_GLEVEL} (0=函数名栈 / 1=含文件:行号，见 ASAN_GLEVEL 说明)"
    echo "  • Link on target needs: LINK_EXTRA_FLAGS='-fsanitize=address,undefined' (deploy.sh 自动带)"
    if [ "$BUILD_OBJECT_ONLY" = "ON" ]; then
        echo "  • Link step: skipped (object-only)"
        echo "Object files are ready for target-machine linking."
    else
        echo "Start server with: ./build/imapsServer"
    fi
else
    echo -e "${GREEN}SafeRelease Mode Enabled:${NC}"
    echo "  • Optimization: -O2 (lower compile memory pressure)"
    echo "  • Tuning: -mtune=generic"
    echo "  • Debug logs: disabled"
    echo "  • Default jobs: 1 (unless manually overridden)"
    echo ""
    if [ "$BUILD_OBJECT_ONLY" = "ON" ]; then
        echo "  • Link step: skipped (object-only)"
        echo "Object files are ready for target-machine linking."
    else
        echo "Start server with: ./build/smtpsServer"
    fi
fi
