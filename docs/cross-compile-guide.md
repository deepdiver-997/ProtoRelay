# 交叉编译部署指南

## 架构

```
macOS ARM64 (开发机)              Linux x86_64 (服务器 120.24.169.213)
┌─────────────────────┐           ┌──────────────────────────┐
│ build.sh cross-x64  │  rsync   │ link.sh → imapsServer    │
│ → .o 文件 (ELF)     │ ──────→ │ → systemctl restart      │
│ artifacts/linux-*/  │  .o文件  │ 服务器本地 g++-13 链接    │
└─────────────────────┘           └──────────────────────────┘
```

- **编译**：macOS 上用 `x86_64-linux-gnu-g++` 交叉编译生成 `.o` 文件
- **链接**：`.o` 上传到服务器，用服务器本地 g++-13 链接（库 ABI 匹配）
- **原因**：macOS Homebrew 的 spdlog/fmt 版本与 Ubuntu 22.04 不同，交叉链接会失败

## 完整流程

### 1. 编译

```bash
# 先准备服务器头文件（首次或服务器更新后需要）
ssh root@120.24.169.213 "tar -czf /tmp/server-headers.tar.gz \
  /usr/include/spdlog /usr/include/fmt"
scp root@120.24.169.213:/tmp/server-headers.tar.gz /tmp/
tar -xzf /tmp/server-headers.tar.gz -C /tmp/server-sysroot/usr/include/

# 用服务器头文件覆盖交叉编译器 sysroot 中的版本
SYSROOT=$(x86_64-linux-gnu-g++ -print-sysroot)/usr/include
cp -r /tmp/server-sysroot/usr/include/spdlog $SYSROOT/
cp -r /tmp/server-sysroot/usr/include/fmt $SYSROOT/

# 交叉编译（必须先禁用 Homebrew 的 spdlog/fmt 查找）
mv /opt/homebrew/lib/cmake/spdlog /opt/homebrew/lib/cmake/spdlog.bak
mv /opt/homebrew/include/spdlog /opt/homebrew/include/spdlog.bak
mv /opt/homebrew/include/fmt /opt/homebrew/include/fmt.bak

EXTRA_CMAKE_ARGS="-DENABLE_S3_STORAGE=OFF" bash build.sh Release clean cross-x64 8

# 恢复 Homebrew
mv /opt/homebrew/lib/cmake/spdlog.bak /opt/homebrew/lib/cmake/spdlog
mv /opt/homebrew/include/spdlog.bak /opt/homebrew/include/spdlog
mv /opt/homebrew/include/fmt.bak /opt/homebrew/include/fmt
```

- `ENABLE_S3_STORAGE=OFF` 必须设置，交叉编译器没有 libcurl
- `build.sh cross-x64` 自动使用 `x86_64-linux-gnu-g++` 并跳过链接
- 产物在 `artifacts/linux-x86_64/Release/obj/`

### 2. 上传

```bash
rsync -avz artifacts/linux-x86_64/Release/obj/ root@120.24.169.213:/opt/smtpServer/build-obj/
```

### 3. 服务器链接

```bash
ssh root@120.24.169.213 "
cd /opt/smtpServer/build-obj
chmod +x link.sh

# 删除冲突的 test entry points（避免 multiple main）
rm -f CMakeFiles/mailServer_obj.dir/test/mail_server_combined.cpp.o
rm -f CMakeFiles/smtpsServer_obj.dir/test/smtps_test.cpp.o

# 链接 imapsServer
./link.sh CMakeFiles/imapsServer_obj.dir/test/imaps_test.cpp.o \
  -o /tmp/imapsServer_new --compiler g++-13

# 部署
systemctl stop imapserver.service
mv /tmp/imapsServer_new /opt/smtpServer/imapsServer
chmod +x /opt/smtpServer/imapsServer
systemctl start imapserver.service
"
```

- link.sh 自动找到所有 `.o` 文件并链接
- 必须删除 smtps_test.cpp.o 和 mail_server_combined.cpp.o，否则 `multiple definition of main`
- 用 `--compiler g++-13` 指定服务器编译器

### 4. 快速部署（smtpsServer）

```bash
# 同上，但入口文件是 smtps_test.cpp.o
./link.sh CMakeFiles/smtpsServer_obj.dir/test/smtps_test.cpp.o \
  -o /tmp/smtpsServer_new --compiler g++-13
```

## 常见问题

### spdlog/fmt ABI 不匹配

**现象**：链接时报 `undefined reference to spdlog::details::log_msg::log_msg(..., fmt::v12::...)`

**原因**：macOS Homebrew 的 spdlog 使用 fmt v12，服务器 Ubuntu 22.04 使用 fmt v8（libfmt8）

**解决**：编译前把服务器的 spdlog/fmt 头文件覆盖到交叉编译器 sysroot（见上面步骤1）

### Boost awaitable.hpp std::exchange 错误

**现象**：服务器直接编译时 `error: 'exchange' is not a member of 'std'`

**原因**：Boost 1.74 + g++-13 的兼容性 bug

**解决**：不要服务器直接编译。交叉编译的 Boost 版本无此问题。如果必须服务器编译，加 `-DBOOST_ASIO_DISABLE_AWAITABLE`

### multiple definition of main

**现象**：链接时报 `multiple definition of 'main'`

**原因**：link.sh 包含了所有目标的 `.o` 文件，包括 smtps_test.cpp.o 和 mail_server_combined.cpp.o

**解决**：链接前删除不相关的 test entry point `.o` 文件

### S3 storage undefined reference

**现象**：链接时报 `undefined reference to S3StorageProvider::S3StorageProvider(...)`

**原因**：交叉编译器没有 libcurl，S3 模块被跳过

**解决**：编译时加 `-DENABLE_S3_STORAGE=OFF`，代码中已有 `#ifdef ENABLE_S3_STORAGE` 保护

### 服务器编译卡死

**现象**：服务器上 `cmake --build` 内存耗尽

**原因**：服务器只有 2GB RAM，并行编译消耗大

**解决**：不要服务器编译，用交叉编译。交叉编译只生成 `.o`，链接在服务器上做（内存消耗小）

## 关键文件

| 文件 | 作用 |
|------|------|
| `build.sh` | 编译脚本，`cross-x64` 模式启用交叉编译 |
| `artifacts/linux-x86_64/Release/obj/link.sh` | 服务器端链接脚本 |
| `config/imapsConfig.json` | 服务器 IMAP 配置 |
| `/opt/smtpServer/` | 服务器部署目录 |
| `/etc/systemd/system/imapserver.service` | systemd 服务定义 |
