#!/bin/bash
# 一键全套单测：构建（可跳过）+ 逐个运行 CMake 注册的单测 + 汇总结果。
#
# 用法：
#   bash test/run_all_tests.sh                # 增量构建 Release + 全套运行
#   bash test/run_all_tests.sh Debug          # 指定构建类型
#   bash test/run_all_tests.sh --no-build     # 跳过构建，只跑已有二进制
#
# 环境变量：
#   RUN_DB_TESTS=0   跳过依赖真实 MySQL 的测试（默认 1 = 包含；本机已配 db_config 时应保持 1）
#   TEST_LOG_DIR     单测日志目录（默认 build/test-logs，随构建目录隔离，多 worktree 并行不互相覆盖）
#
# 退出码：0 = 全部通过；1 = 有失败/构建失败。失败详情在 $TEST_LOG_DIR/<test>.log。
# 范围说明：本脚本覆盖 C++ 单测（test/unit）；e2e/bench/fuzz 不在其中
# （e2e 需起真实服务+DB，见 test/e2e；bench 见 test/bench）。

set -u
cd "$(dirname "$0")/.." || exit 1

BUILD_TYPE=Release
BUILD_DIR="${BUILD_DIR:-build}"
NO_BUILD=0
for a in "$@"; do
    case "$a" in
        Debug|Release|SafeRelease) BUILD_TYPE="$a" ;;
        --no-build|-n) NO_BUILD=1 ;;
        *) echo "未知参数: $a（支持 <Debug|Release|SafeRelease> / --no-build）"; exit 2 ;;
    esac
done

if [ "$NO_BUILD" -eq 0 ]; then
    bash build.sh "$BUILD_TYPE" || { echo "BUILD FAILED"; exit 1; }
fi

if [ ! -d "$BUILD_DIR" ]; then
    echo "$BUILD_DIR 不存在，先去掉 --no-build 跑一次构建"; exit 1
fi

RUN_DB_TESTS="${RUN_DB_TESTS:-1}"
SKIP_DB_RE='^(mariadb_async_test)$'
LOG_DIR="${TEST_LOG_DIR:-$BUILD_DIR/test-logs}"
mkdir -p "$LOG_DIR"

pass=0; fail=0; skip=0
failed_tests=()

# CTest is the authoritative test inventory: stale binaries never enter the run,
# and an unbuilt registered target is a failure rather than a silent omission.
registered_tests=$(ctest --test-dir "$BUILD_DIR" -N 2>/dev/null | sed -nE 's/^[[:space:]]*Test[[:space:]]*#[0-9]+:[[:space:]]*([^[:space:]]+).*$/\1/p')
if [ -z "$registered_tests" ]; then
    echo "未发现 CTest 注册的单测，请先运行 ./build.sh Release"
    exit 1
fi
for name in $registered_tests; do
    if [ "$RUN_DB_TESTS" = "0" ] && [[ "$name" =~ $SKIP_DB_RE ]]; then
        echo "SKIP  $name（依赖真实 MySQL，RUN_DB_TESTS=1 开启）"
        skip=$((skip+1))
        continue
    fi
    if ctest --test-dir "$BUILD_DIR" --output-on-failure -R "^${name}$" > "$LOG_DIR/${name}.log" 2>&1; then
        if grep -q '\*\*\*Skipped' "$LOG_DIR/${name}.log"; then
            echo "SKIP  $name（见 $LOG_DIR/${name}.log）"
            skip=$((skip+1))
        else
            echo "PASS  $name"
            pass=$((pass+1))
        fi
    else
        rc=$?
        echo "FAIL  $name (exit $rc; log: $LOG_DIR/${name}.log)"
        fail=$((fail+1))
        failed_tests+=("$name")
    fi
done

echo
echo "======================================="
echo "全套单测: $pass 通过, $fail 失败, $skip 跳过"
if [ "$fail" -gt 0 ]; then
    printf '失败: %s\n' "${failed_tests[@]}"
    exit 1
fi
echo "ALL GREEN"
