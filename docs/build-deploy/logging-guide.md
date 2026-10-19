# 日志系统配置说明

> **新增**：日志压缩工具链支持构建时将 `LOG_*` 宏替换为 hash+参数的结构化输出，
> 大幅减小日志体积。详见 [log-transform-toolchain.md](log-transform-toolchain.md)。

## 概述

项目使用自己的 `LogLevel`、`LogRecord` 和 `LogSink` 接口。默认提供 spdlog 适配器，也可使用纯标准库输出或关闭默认输出；使用者可以注入自己的记录方法。

## 后端选择与自定义回调

CMake 参数 `PROTORELAY_LOG_BACKEND` 支持 `SPDLOG`（默认）、`STANDARD` 和 `NONE`。
后两者不查找、不包含、不链接 spdlog 或 fmt。`STANDARD` 支持控制台、文件、滚动和 flush；
`NONE` 关闭默认输出，仍可注入自定义处理器。`log_to_console=false` 且 `log_to_file=false`
会关闭默认输出。`max_file_size=0` 表示不滚动。

```bash
cmake -S . -B build/standard -DPROTORELAY_LOG_BACKEND=STANDARD -DPROTORELAY_WORKER_BACKEND=STD
cmake --build build/standard --target smtpsServer
```

在装配服务前注册回调，后续 `init()` / `bootstrap::assemble()` 会保留该回调：

```cpp
#include "framework/log/logger.h"

pr::Logger::get_instance().set_handler([](const pr::LogRecord& record) {
    // 转交给自己的日志函数；record 包含 level/module/message/source/timestamp。
    my_log(record.message);
}, [] { my_log_flush(); });
pr::Logger::get_instance().set_level(pr::LogLevel::info);
```

也可以实现 `pr::LogSink` 并调用 `set_sink()`。传空回调关闭输出。
回调可被多个线程并发调用，使用者负责同步自身状态；回调执行期间不持日志注册表锁。
`LogRecord` 拥有消息和模块字符串，可复制后异步使用；源位置字符串需具有静态生命周期。
处理器和 flush 抛出的异常被隔离并计入 `errors_total()`。
全局级别更新保留模块覆盖；模块可在首次产生日志前设置级别。
`shutdown()` 停止新日志并 flush 当前后端，调用方应先停止并 join 产生日志的线程。

日志格式化独立于输出后端，目前支持顺序 `{}` 参数和 `{{` / `}}` 转义，
覆盖项目现有调用，不提供完整 fmt 格式语法；无效格式计入 `errors_total()`。
`LOG_PURE` 保留 `0xhash|参数...` 格式，也不依赖 spdlog/fmt。

## 编译时配置

### 启用/禁用调试日志

通过 CMake 编译选项控制：

```bash
# 构建时禁用所有调试日志（默认）
cmake -B build

# 构建时启用所有模块的调试日志
cmake -DENABLE_DEBUG_LOGS=ON -B build
```

### 模块化日志控制

在 `logger.h` 中定义了以下宏，编译时控制各模块的调试日志输出：

```cpp
// 在 logger.h 中定义的宏（默认值）
#define ENABLE_SERVER_DEBUG_LOG 0         // 服务器基础日志
#define ENABLE_NETWORK_DEBUG_LOG 0        // 网络连接日志
#define ENABLE_DATABASE_DEBUG_LOG 0        // 数据库连接池日志
#define ENABLE_DATABASE_QUERY_DEBUG_LOG 0  // 数据库查询详细日志（通常关闭）
#define ENABLE_SMTP_DEBUG_LOG 0           // SMTP 协议日志
#define ENABLE_SMTP_DETAIL_DEBUG_LOG 0     // SMTP 状态机详细日志（通常关闭）
#define ENABLE_SESSION_DEBUG_LOG 0         // 会话管理日志
#define ENABLE_THREAD_POOL_DEBUG_LOG 0      // 线程池日志
#define ENABLE_FILE_IO_DEBUG_LOG 0          // 文件 I/O 日志
#define ENABLE_AUTH_DEBUG_LOG 0             // 认证日志
```

**注意**：这些宏使用 `constexpr`，在编译时决定是否生成代码，**不会影响运行时性能**。

## 运行时配置

### 全局日志级别

在 `test/server/smtps_test.cpp` 的 `main()` 函数中初始化日志系统：

```cpp
Logger::get_instance().init(
    "logs/mail_system.log",           // 日志文件路径
    1024 * 1024 * 5,               // 单个文件最大大小 (5MB)
    3,                               // 保留文件数量
    pr::LogLevel::info              // 全局日志级别
);
```

### 日志级别

项目支持以下日志级别（从低到高）：

| 级别 | 说明 | 使用场景 |
|--------|------|----------|
| `trace` | 最详细的跟踪信息 | 开发调试，跟踪代码执行路径 |
| `debug` | 调试信息 | 开发调试，查看变量值和状态 |
| `info` | 一般信息 | 正常运行信息，如连接建立、邮件发送等 |
| `warn` | 警告信息 | 潜在问题，如数据库连接慢、重试等 |
| `error` | 错误信息 | 运行时错误，需要关注但不影响系统运行 |
| `critical` | 严重错误 | 系统无法正常运行，需要立即处理 |

**建议**：
- 生产环境使用 `pr::LogLevel::info`
- 调试时使用 `pr::LogLevel::debug` 或 `pr::LogLevel::trace`
- 发布版本使用 `pr::LogLevel::warn` 或 `pr::LogLevel::err`

### 运行时动态调整日志级别

在代码中可以动态调整日志级别：

```cpp
// 设置全局日志级别
mail_system::set_log_level(pr::LogLevel::debug);

// 设置特定模块的日志级别
mail_system::set_module_log_level("DATABASE", pr::LogLevel::trace);
```

## 模块化日志使用

### 日志模块分类

| 模块 | 宏前缀 | 日志文件标识 | 使用场景 |
|--------|----------|----------------|----------|
| SERVER | `LOG_SERVER_*` | SERVER | 服务器启动、停止、配置加载 |
| NETWORK | `LOG_NETWORK_*` | NETWORK | TCP/SSL 连接接受、连接状态 |
| DATABASE | `LOG_DATABASE_*` | DATABASE | 数据库连接池管理、连接创建/销毁 |
| DB_QUERY | `LOG_DB_QUERY_*` | DB_QUERY | SQL 语句执行、prepared statement |
| SMTP | `LOG_SMTP_*` | SMTP | SMTP 协议命令处理（EHLO, MAIL FROM 等） |
| SMTP_DETAIL | `LOG_SMTP_DETAIL_*` | SMTP_DETAIL | 状态机状态转换、事件处理（通常关闭） |
| SESSION | `LOG_SESSION_*` | SESSION | 会话生命周期、错误处理 |
| THREAD_POOL | `LOG_THREAD_POOL_*` | THREAD_POOL | 线程池启动、停止、任务提交 |
| FILE_IO | `LOG_FILE_IO_*` | FILE_IO | 文件读写、保存邮件正文 |
| AUTH | `LOG_AUTH_*` | AUTH | 用户认证、权限验证 |

### 使用示例

```cpp
// SERVER 模块
LOG_SERVER_INFO("Server initialized with SSL: {}", enabled ? "enabled" : "disabled");
LOG_SERVER_ERROR("Failed to create MySQL pool: {}", e.what());

// NETWORK 模块
LOG_NETWORK_INFO("New SSL connection accepted");
LOG_NETWORK_ERROR("Error accepting SSL connection: {}", ec.message());

// DATABASE 模块（连接池级别）
LOG_DATABASE_DEBUG("Initializing pool");
LOG_DATABASE_WARN("Database pool is not initialized");

// DB_QUERY 模块（SQL 执行级别）
LOG_DB_QUERY_DEBUG("Executing SQL: {}", sql);
LOG_DB_QUERY_ERROR("MySQL query error: {}", mysql_error(m_mysql));

// SMTP 模块
LOG_SMTP_INFO("Mail received from: {}", sender);
LOG_SMTP_ERROR("Failed to parse mail address: {}", address);

// SMTP_DETAIL 模块（仅在需要调试状态机时启用）
LOG_SMTP_DETAIL_INFO("Current State: {}, Event: {}", state, event);
LOG_SMTP_DETAIL_WARN("No handler for state {} and event {}", state, event);
```

## 日志输出格式

### 默认格式

```
[2026-01-05 10:30:45.123] [info] [12345] [SERVER] Server initialized with SSL: enabled
```

格式说明：
- `[时间]` - 精确到毫秒的时间戳
- `[级别]` - 日志级别（trace/debug/info/warn/error/critical）
- `[线程ID]` - 输出日志的线程 ID
- `[模块名]` - 日志所属模块
- `消息` - 日志内容

### 格式化输出

使用 spdlog 的格式化语法：

```cpp
// 位置参数 {}
LOG_SERVER_INFO("Server started on port {}", port);

// 多个参数
LOG_SERVER_INFO("Server initialized with SSL: {}, TCP: {}", ssl_enabled, tcp_enabled);

// 十六进制输出
LOG_NETWORK_DEBUG("Connection ID: {:x}", connection_id);
```

## 日志文件管理

### 文件滚动

日志文件自动滚动：
- 单个文件最大 5MB（可在 `init()` 中修改）
- 保留最近 3 个文件
- 超出后自动删除旧文件

### 文件输出

- 同时输出到**终端**（彩色）和**文件**（纯文本）
- 终端输出：带颜色区分不同级别
- 文件输出：`logs/mail_system.log`

## 调试建议

### 场景 1：调试连接池问题

```bash
# 重新编译，启用数据库调试日志
cmake -DENABLE_DEBUG_LOGS=ON -B build
make

# 运行服务器
./build/smtpsServer

# 查看日志
tail -f logs/mail_system.log | grep DB_QUERY
```

### 场景 2：生产环境

```bash
# 正常编译（禁用详细日志）
cmake -B build
make

# 运行服务器
./build/smtpsServer

# 只查看错误日志
tail -f logs/mail_system.log | grep -E "(error|critical)"
```

### 场景 3：调试 SMTP 状态机

```bash
# 修改 logger.h，仅启用 SMTP_DETAIL
#define ENABLE_SMTP_DETAIL_DEBUG_LOG 1

# 重新编译
cmake -B build && make

# 查看状态机转换
tail -f logs/mail_system.log | grep SMTP_DETAIL
```

## 常用日志过滤命令

```bash
# 查看实时日志
tail -f logs/mail_system.log

# 过滤特定模块
tail -f logs/mail_system.log | grep -E "(SERVER|NETWORK)"

# 过滤错误和警告
tail -f logs/mail_system.log | grep -E "(error|warn)"

# 统计错误数量
grep -c "error" logs/mail_system.log

# 查看最近的错误
grep "error" logs/mail_system.log | tail -20
```

## 配置示例

### 开发环境

```cpp
// smtps_test.cpp
Logger::get_instance().init(
    "logs/mail_system.log",
    1024 * 1024 * 10,  // 10MB 单文件，方便开发调试
    10,                  // 保留 10 个文件
    pr::LogLevel::debug   // debug 级别
);
```

### 生产环境

```cpp
// smtps_test.cpp
Logger::get_instance().init(
    "logs/mail_system.log",
    1024 * 1024 * 5,   // 5MB 单文件
    3,                    // 保留 3 个文件
    pr::LogLevel::info    // info 级别
);
```

### 调试特定问题

```cpp
// smtps_test.cpp
Logger::get_instance().init(
    "logs/mail_system.log",
    1024 * 1024 * 5,
    3,
    pr::LogLevel::trace   // 最详细的级别
);

// 如果只关心数据库问题，运行时调整
mail_system::set_module_log_level("DATABASE", pr::LogLevel::trace);
mail_system::set_module_log_level(mail_system::LogModule::DB_QUERY, pr::LogLevel::trace);
```
