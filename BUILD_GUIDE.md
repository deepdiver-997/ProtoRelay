# 构建配置指南

## 概述

直接 CMake 使用 Debug/Release，并提供 ASan/UBSan、TSan、coverage 和 object-only 开关；
`build.sh` 另提供 SafeRelease/Asan 预设。脚本按 target 串行、target 内并行，避免内存峰值。

## 构建模式对比

| 特性 | Debug 模式 | Release 模式 |
|------|-----------|------------|
| **优化级别** | `-O0`（无优化） | `-O3`（高优化） |
| **本地优化** | ❌ | ✅ `-march=native` |
| **调试符号** | ✅ `-g` | ❌ |
| **帧指针** | ✅ `-fno-omit-frame-pointer` | ❌ |
| **NDEBUG** | ❌ | ✅ |
| **日志级别** | DEBUG（全部） | INFO（INFO/WARNING/ERROR） |
| **编译时间** | 快 | 较慢 |
| **运行时速度** | 慢 | 快 |
| **二进制大小** | 大 | 小 |

新增低内存兜底模式（`SafeRelease`）：
- 单线程构建（默认 `-j1`）
- 降低优化压力（`-O2`）
- 关闭调试日志编译开关
- 适用于 2核2G 等低内存服务器

## 快速开始

### 使用 build.sh 脚本

```bash
# Debug 构建（推荐用于开发/调试）
./build.sh Debug

# Release 构建（推荐用于测试/部署）
./build.sh Release

# SafeRelease 构建（低内存服务器推荐）
./build.sh SafeRelease

# SafeRelease 清理并重建
./build.sh SafeRelease clean

# 清理并重新构建
./build.sh Debug clean
./build.sh Release clean

# 指定并行度（可选，SafeRelease 默认就是 1）
./build.sh Release clean 1
BUILD_JOBS=1 ./build.sh Release
```

### 手动 CMake 命令

```bash
# Debug 模式
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j$(nproc)
cd ..

# Release 模式
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
cd ..
```

## 路径自动推导

CMakeLists.txt 现已支持自动查找依赖库，无需硬编码路径：

### MySQL
1. **优先级 1**: 通过 pkg-config 查找
2. **优先级 2**: 自动检测常见路径：
   - **macOS (Apple Silicon)**: `/opt/homebrew/opt/mysql-client`, `/opt/homebrew`
   - **macOS (Intel)**: `/usr/local/opt/mysql`, `/usr/local`
   - **Linux**: `/usr`, `/usr/local`, `/opt/mysql`

### OpenSSL
- 通过 CMake 的 `find_package` 自动查找
- macOS 回退路径：`/opt/homebrew/opt/openssl`, `/usr/local/opt/openssl`

## 日志配置

### Debug 日志（独立开关）

直接 `cmake -DCMAKE_BUILD_TYPE=Debug` 不会自动打开所有 DEBUG 宏；需显式
`-DENABLE_DEBUG_LOGS=ON`。`build.sh Debug` 预设会设置该开关。启用后包括：
- `ENABLE_SERVER_DEBUG_LOG=1`
- `ENABLE_NETWORK_DEBUG_LOG=1`
- `ENABLE_DATABASE_DEBUG_LOG=1`
- `ENABLE_DATABASE_QUERY_DEBUG_LOG=1`
- `ENABLE_SMTP_DEBUG_LOG=1`
- `ENABLE_SMTP_DETAIL_DEBUG_LOG=1`
- `ENABLE_SESSION_DEBUG_LOG=1`
- `ENABLE_THREAD_POOL_DEBUG_LOG=1`
- `ENABLE_FILE_IO_DEBUG_LOG=1`
- `ENABLE_AUTH_DEBUG_LOG=1`

### Release 模式（自动禁用）
所有 DEBUG 日志关闭，仅保留 INFO、WARNING、ERROR 级别。

## 手动覆盖日志设置

即使在 Release 模式下，也可以强制启用 DEBUG 日志：

```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DENABLE_DEBUG_LOGS=ON ..
make -j$(nproc)
```

## 编译警告和诊断

启用的编译选项：
- `-Wall`: 所有常见警告
- `-Wextra`: 额外警告
- `-Wno-unused-parameter`: 抑制未使用参数警告（常见于模板代码）
- `-fdiagnostics-color=auto`: 彩色诊断输出（GCC/Clang）

## 性能优化 (Release)

是否出现 `-march=native` 取决于 CMake 缓存与构建入口，不承诺固定性能收益。
部署到不同 CPU 时必须显式指定兼容 ISA，例如 `-DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -march=x86-64-v2"`，
不能把本机 native 优化产物直接发往旧 CPU。生产标准采用目标 ABI 一致的本地容器构建，
见 [部署操作](docs/build-deploy/operations.md)；服务器只接收成品。

## 输出目录结构

```
mail-system/v8/
├── build/
│   ├── CMakeFiles/     # 中间产物 (object files, etc.)
│   ├── smtpsServer     # SMTP 服务器可执行文件
│   ├── imapsServer     # IMAP 服务器可执行文件
│   └── ...
├── test/
│   ├── unit/           # C++ 单元测试
│   ├── bench/          # 性能基准
│   ├── e2e/            # Python 端到端测试
│   └── server/         # 服务器入口 main()
├── CMakeLists.txt
└── build.sh
```

## 故障排除

### MySQL 库未找到

如果 CMake 无法自动找到 MySQL，手动指定路径：

```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release \
      -DMYSQL_INCLUDE_DIRS=/opt/homebrew/include \
      -DMYSQL_LIBRARIES=/opt/homebrew/lib/libmysqlclient.dylib \
      ..
make -j$(nproc)
```

### OpenSSL 库未找到（macOS）

```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release \
      -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl \
      ..
make -j$(nproc)
```

## 测试运行

### 冒烟测试（Python cl.py，支持 TLS/AUTH）
```bash
# 启动服务器
./build/smtpsServer -c config/smtpsConfig.json

# 另一个终端：Python 冒烟测试
cd test/scripts
python3 cl.py --mode mta-relay --messages 10 --concurrency 2 --verbose
```

### 性能压测（C++ smtp_client，推荐）
```bash
# 启动服务器
./build/smtpsServer -c config/smtpsConfig.json

# 全矩阵压测（详见 test/bench/bench-report.md）
./build/smtp_client --pipe --reuse --t 32 --msgs 50000   # 最大吞吐
./build/smtp_client --seq --reuse --t 16 --msgs 5000     # MTA 中继模拟
./build/smtp_client --pipe --t 4 --msgs 5000             # pipeline per-conn
./build/smtp_client --seq --t 8 --msgs 5000              # sequential per-conn
```

## 核对产物与前端

```bash
./build-release/smtpsServer --version
./build-release/imapsServer --version
ctest --test-dir build-release -N
npm ci --prefix webmail
npm run --prefix webmail build
```

构建输出由 `-B` 或脚本 BUILD_DIR 决定，不能用历史输出示例推断实际目录。
产物应报告 Git commit、UTC 构建时间和目标架构；容器需装 git 才能注入真实提交号。
前端源码与 C++ 构建分开，产物在 `build-webmail/`，详见 [前端说明](webmail/README.md)。
