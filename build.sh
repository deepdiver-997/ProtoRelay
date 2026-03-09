#!/bin/bash
# 构建脚本 - 支持 Debug / Release / SafeRelease 模式

set -e  # 任何命令失败则停止

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${SCRIPT_DIR}/build}"
ARTIFACT_DIR="${ARTIFACT_DIR:-${SCRIPT_DIR}/artifacts}"
EXTRA_CMAKE_ARGS_STR="${EXTRA_CMAKE_ARGS:-}"
GENERATOR="${CMAKE_GENERATOR:-}"

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

# 使用方法
if [ "$#" -lt 1 ]; then
    echo "Usage: $0 <Debug|Release|SafeRelease> [clean] [jobs] [object-only] [cross-x64]"
    echo ""
    echo "Examples:"
    echo "  $0 Debug           # 构建 Debug 版本（无优化，启用所有调试日志）"
    echo "  $0 Release         # 构建 Release 版本（高优化，仅 INFO 级别日志）"
    echo "  $0 SafeRelease     # 低内存兜底构建（2核2G服务器推荐）"
    echo "  $0 Debug clean     # 清理后重新构建 Debug 版本"
    echo "  $0 Release clean   # 清理后重新构建 Release 版本"
    echo "  $0 Release clean 1 # 单线程构建（低内存服务器推荐）"
    echo "  $0 SafeRelease clean"
    echo "  $0 Release clean 1 object-only   # 仅编译 .o，不做最终链接"
    echo "  $0 Debug object-only             # 快速验证编译（需头文件，不强制链接库）"
    echo "  $0 Release clean 1 cross-x64 object-only  # 交叉编译到 Linux x86_64（推荐）"
    echo ""
    echo "Env override: BUILD_JOBS=<n>, EXTRA_CMAKE_ARGS='<...>'"
    echo "              ARTIFACT_DIR=<path>, CROSS_CC=<path>, CROSS_CXX=<path>"
    echo ""
    exit 1
fi

BUILD_TYPE="$1"
CLEAN_BUILD=""
USER_JOBS=""
BUILD_OBJECT_ONLY="OFF"
CROSS_X64_LINUX="OFF"
CROSS_CC="${CROSS_CC:-}"
CROSS_CXX="${CROSS_CXX:-}"

# 解析可选参数，顺序不限：clean / <jobs> / object-only / cross-x64
for arg in "${@:2}"; do
    if [[ "$arg" == "clean" ]]; then
        CLEAN_BUILD="clean"
    elif [[ "$arg" =~ ^[0-9]+$ ]]; then
        USER_JOBS="$arg"
    elif [[ "$arg" == "object-only" || "$arg" == "obj-only" || "$arg" == "nolink" ]]; then
        BUILD_OBJECT_ONLY="ON"
    elif [[ "$arg" == "cross-x64" || "$arg" == "cross-linux-x64" || "$arg" == "cross-ubuntu24" || "$arg" == "x64-ubuntu24" ]]; then
        CROSS_X64_LINUX="ON"
    else
        print_warning "Unknown argument: $arg"
        echo "Allowed optional args: clean, <jobs>, object-only, cross-x64"
        exit 1
    fi
done

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
if [[ "$BUILD_TYPE" != "Debug" && "$BUILD_TYPE" != "Release" && "$BUILD_TYPE" != "SafeRelease" ]]; then
    print_warning "Invalid build type: $BUILD_TYPE"
    echo "Must be 'Debug', 'Release' or 'SafeRelease'"
    exit 1
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

# 清理构建目录（可选）
if [ "$CLEAN_BUILD" = "clean" ]; then
    print_info "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
fi

# 创建构建目录
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

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

if [[ "$CROSS_X64_LINUX" == "ON" ]]; then
    resolve_cross_compiler "$CROSS_CC" "$CROSS_CXX"

    if [[ "$BUILD_OBJECT_ONLY" != "ON" ]]; then
        print_warning "cross-x64 defaults to object-only to avoid host/target link mismatch"
        BUILD_OBJECT_ONLY="ON"
    fi
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
)

if [[ "$CROSS_X64_LINUX" == "ON" ]]; then
    CROSS_SYSROOT="$($CROSS_CXX -print-sysroot 2>/dev/null || true)"
    cmake_args+=(
        -DCMAKE_SYSTEM_NAME=Linux
        -DCMAKE_SYSTEM_PROCESSOR=x86_64
        -DCMAKE_CXX_COMPILER="$CROSS_CXX"
    )
    if [ -n "$CROSS_SYSROOT" ]; then
        cmake_args+=( -DCMAKE_SYSROOT="$CROSS_SYSROOT" )
    fi
fi

if [ -n "$GENERATOR" ]; then
    cmake_args+=( -G "$GENERATOR" )
fi

if [ -n "$EXTRA_CMAKE_ARGS_STR" ]; then
    # shellcheck disable=SC2206
    user_extra_args=( $EXTRA_CMAKE_ARGS_STR )
    cmake_args+=( "${user_extra_args[@]}" )
fi

cmake_args+=( "${SAFE_CMAKE_ARGS[@]}" )

cmake "${cmake_args[@]}"

# 编译
print_info "Building with ${BUILD_JOBS_VALUE} thread(s)..."
cmake --build "$BUILD_DIR" -- -j"$BUILD_JOBS_VALUE"

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
    print_info "Executable: ${SCRIPT_DIR}/test/smtpsServer"
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
mkdir -p "$EXPORT_DIR"

if [ "$BUILD_OBJECT_ONLY" = "ON" ]; then
    OBJ_ROOT="${BUILD_DIR}/CMakeFiles/smtpsServer_obj.dir"
    if [ -d "$OBJ_ROOT" ]; then
        while IFS= read -r obj; do
            rel="${obj#${OBJ_ROOT}/}"
            out_dir="${EXPORT_DIR}/$(dirname "$rel")"
            mkdir -p "$out_dir"
            cp "$obj" "$out_dir/"
        done < <(find "$OBJ_ROOT" -type f -name '*.o' | sort)
    fi

    # 把链接脚本也放进去，目标机可直接复用。
    if [ -f "${SCRIPT_DIR}/link.sh" ]; then
        cp "${SCRIPT_DIR}/link.sh" "${EXPORT_DIR}/"
        chmod +x "${EXPORT_DIR}/link.sh"
    fi

    OBJ_COUNT=$(find "$EXPORT_DIR" -type f -name '*.o' | wc -l | tr -d ' ')
    print_info "Exported object files: ${OBJ_COUNT}"
else
    if [ -f "${SCRIPT_DIR}/test/smtpsServer" ]; then
        cp "${SCRIPT_DIR}/test/smtpsServer" "${EXPORT_DIR}/"
        chmod +x "${EXPORT_DIR}/smtpsServer"
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
        echo "Start server with: ./test/smtpsServer"
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
        echo "Start server with: ./test/smtpsServer"
        echo "Or run tests with: cd test && uv run cl.py"
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
        echo "Start server with: ./test/smtpsServer"
    fi
fi
