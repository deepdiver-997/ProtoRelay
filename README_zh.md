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