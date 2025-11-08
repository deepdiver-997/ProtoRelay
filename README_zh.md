# Mail System（学习项目）

本仓库包含一个基于 C++、Boost.Asio 和 OpenSSL 的简易邮件服务器（包含 SMTPS/IMAPS 相关代码），用于学习和实验目的，不适用于生产环境。

## 概览

- 语言：C++17
- 网络库：Boost.Asio
- TLS：OpenSSL
- 线程模型：自定义线程池（基于 Boost 和 IO-context 的实现）
- 数据库：引用了 MySQL 客户端，但数据库持久化尚未完善（见下面说明）

## 使用 CMake 构建（推荐）

在项目根目录下执行：

```bash
# 生成 build 目录并配置
cmake -S . -B build
# 构建 SMTPS 可执行文件
cmake --build build --target smtpsServer -j2
# 或构建 IMAPS
cmake --build build --target imapsServer -j2
```

默认 CMake 配置会将中间构建产物放在 `build/`，并把最终可执行文件放到项目根的 `test/` 目录（例如 `test/smtpsServer`）。

如果你更喜欢使用旧的 Makefile，仓库中也保留了 `build/MakeFile`，但推荐使用 CMake。

## 平台依赖（安装示例）

本项目依赖 C++17、CMake、Boost、OpenSSL 与 MySQL 客户端库。下面给出常见平台的安装示例，按需调整。

Linux (Ubuntu/Debian)：

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config libssl-dev libboost-all-dev libmysqlclient-dev
```

macOS (Homebrew)：

```bash
brew update
brew install cmake openssl boost mysql pkg-config
# macOS 上可能需要在 cmake 时通过 -DOPENSSL_ROOT_DIR 指定 OpenSSL 路径
```

Windows（推荐使用 Visual Studio + vcpkg）：

1. 安装 Visual Studio（添加 “Desktop development with C++” 工作负载）。
2. 安装 vcpkg 并通过它安装依赖：

```powershell
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
./bootstrap-vcpkg.bat
./vcpkg install boost openssl mysql-client
# 构建时通过 -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake 集成
```

## 配置与构建 示例

基本的 out-of-source 构建：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target smtpsServer -j$(nproc)
```

如果 CMake 未能自动找到 OpenSSL 或 MySQL 库，可以手动传入路径（macOS Homebrew 示例）：

```bash
cmake -S . -B build -DOPENSSL_ROOT_DIR=$(brew --prefix openssl) -DMYSQL_INCLUDE_DIRS=/usr/local/opt/mysql/include -DMYSQL_LIBRARIES=/usr/local/opt/mysql/lib/libmysqlclient.dylib
```

如果 Boost 安装在非标准位置，可通过 `-DBOOST_ROOT=/path/to/boost` 指定。
如果 CMake 无法自动找到 OpenSSL 或 MySQL 库，可以手动传入路径（macOS Homebrew 示例）：

```bash
cmake -S . -B build -DOPENSSL_ROOT_DIR=$(brew --prefix openssl) -DMYSQL_INCLUDE_DIRS=/usr/local/opt/mysql/include -DMYSQL_LIBRARIES=/usr/local/opt/mysql/lib/libmysqlclient.dylib
```

如果 Boost 安装在非标准位置，可通过 `-DBOOST_ROOT=/path/to/boost` 指定：

```bash
cmake -S . -B build -DBOOST_ROOT=/path/to/boost
```

## OpenSSL 许可与说明

本项目依赖 OpenSSL。OpenSSL 拥有自己的许可与归属说明（历史上有 OpenSSL License 与 SSLeay License；OpenSSL 3.x 则采用 Apache License 2.0）。如果你随二进制再分发 OpenSSL，请遵守 OpenSSL 的许可要求并随包附上相应许可文本。详情见：https://www.openssl.org/source/license.html

补充说明：

- OpenSSL 1.1.x 及更早版本使用 OpenSSL License + SSLeay License；OpenSSL 3.x 使用 Apache License 2.0，请根据你使用的 OpenSSL 版本遵守相应许可。
- 如果通过 Homebrew 在 macOS 上安装 OpenSSL（尤其是 Apple Silicon），OpenSSL 通常位于 `/opt/homebrew` 前缀下。可在配置时通过 `-DOPENSSL_ROOT_DIR=$(brew --prefix openssl)` 指定 OpenSSL 路径。

macOS 下 MySQL/Homebrew 的常用 cmake 示例：

```bash
cmake -S . -B build -DMYSQL_INCLUDE_DIRS=/opt/homebrew/include -DMYSQL_LIBRARIES=/opt/homebrew/lib/libmysqlclient.dylib
```

## 运行 SMTPS 服务器

1. 准备 TLS 证书与私钥（`.crt` / `.key`），仓库会忽略这些敏感文件。示例证书可能位于 `src/mail_system/back/crt/`（若存在）。
2. 启动服务器：

```bash
./test/smtpsServer
```

3. 使用 openssl 客户端测试：

```bash
openssl s_client -crlf -connect 127.0.0.1:465
```

## 第三方库与许可说明

- 本项目包含 nlohmann/json 的 single-header 版本（位于 `OuterLib/json`），该库使用 MIT 许可。

署名：

- nlohmann/json — JSON for Modern C++（single-header）
    - 仓库: https://github.com/nlohmann/json
    - 许可: MIT License

## 关于 Boost 许可

本项目使用 Boost 库。如需再分发二进制或 vendor Boost 头文件，请随包附上 Boost Software License 文本。仓库根目录包含 `COPYING_BOOST.txt`。

## 配置文件

配置信息存放在 `config/` 下（例如 `smtpsConfig.json`、`imapsConfig.json`、`db_config.json`）。运行前请确认地址、端口和证书路径正确。

## 故障排查

- 如果出现 FindBoost 相关的 CMake 警告，可通过安装系统包或使用 vcpkg 提供的 CMake target 来消除警告。
- 如果找不到 MySQL 客户端，先安装 `pkg-config` 与 `libmysqlclient`，或在 cmake 调用时手动指定 `MYSQL_INCLUDE_DIRS` 和 `MYSQL_LIBRARIES`。
# Mail System（学习项目）

本仓库包含一个基于 C++、Boost.Asio 和 OpenSSL 的简易邮件服务器（包含 SMTPS/IMAPS 相关代码），用于学习和实验目的，不适用于生产环境。

## 概览

- 语言：C++17
- 网络库：Boost.Asio
- TLS：OpenSSL
- 线程模型：自定义线程池（基于 Boost 和 IO-context 的实现）
- 数据库：引用了 MySQL 客户端，但数据库持久化尚未完善（见下面说明）

## 使用 CMake 构建（推荐）

在项目根目录下执行：

```bash
# 生成 build 目录并配置
cmake -S . -B build
# 构建 SMTPS 可执行文件
cmake --build build --target smtpsServer -j2
# 或构建 IMAPS
cmake --build build --target imapsServer -j2
```

默认 CMake 配置会将中间构建产物放在 `build/`，并把最终可执行文件放到项目根的 `test/` 目录（例如 `test/smtpsServer`）。

如果你更喜欢使用旧的 Makefile，仓库中也保留了 `build/MakeFile`，但推荐使用 CMake。

## 运行 SMTPS 服务器并测试

1. 确保有 TLS 证书和私钥文件（`.crt` / `.key`）。仓库的 `.gitignore` 会忽略这些私钥/证书文件。示例证书可能位于 `src/mail_system/back/crt/`（若存在）。请在配置文件中指定正确路径。

2. 启动服务器（示例使用端口 465）：

```bash
./test/smtpsServer
```

3. 在另一个终端使用 OpenSSL 客户端进行测试：

```bash
openssl s_client -crlf -connect 127.0.0.1:465
```

然后按以下会话流程与服务器交互（示例）：

```
220 SMTPS Server Ready
helo server
250-server Hello
250-SIZE 10240000
250-8BITMIME
250 SMTPUTF8
mail from: <xxx@abc.com>
250 Ok
rcpt to: <xx@123.com>
250 Ok
data
354 Start mail input; end with <CRLF>.<CRLF>
morning my friend!
.
250 Message accepted for delivery
quit
221 Bye
closed
```

## 重要说明

- 本项目主要用于学习和演示。数据库存储功能尚未完善，因此即使服务器确认接收邮件，邮件也可能不会被写入数据库。
- 如果在启动时数据库不可用，代码已将 DB 池的创建改为异步并设置了超时（默认 5 秒），以避免构造函数长时间阻塞。你可以在日志中看到超时或创建失败的提示。


## 第三方库与许可说明

- 本项目包含 nlohmann/json 的单文件版本（位于 `OuterLib/json/single_include`）用于 JSON 解析。nlohmann/json 使用 MIT 许可发布，我们以 vendor 形式包含其单文件实现以便学习和实验使用。

署名与链接：

- nlohmann/json — JSON for Modern C++（single-header）
	- 仓库: https://github.com/nlohmann/json
	- 许可: MIT License

在二进制分发或再发布本项目时，请务必保留 nlohmann/json 的 MIT 许可文本（单文件头部自带许可信息），以符合集成许可要求。