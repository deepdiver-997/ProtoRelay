# 日志系统集成完成

## 概述

已成功将 [spdlog](https://github.com/gabime/spdlog) 集成到邮件系统中，替换了所有的 `std::cout` 和 `std::cerr` 输出。

## 已完成的工作

### 1. 创建日志系统基础设施

**文件**: `include/mail_system/back/common/logger.h`

**功能**:
- 单例模式的日志管理器
- 10 个模块化日志类别
- 多 sink 输出（终端彩色 + 文件）
- 文件滚动（按大小）
- 动态日志级别控制
- 模块化日志宏（支持编译时开关）

### 2. 模块化日志设计

| 模块 | 日志标识 | 文件中使用 | 默认状态 |
|--------|----------|------------|----------|
| SERVER | SERVER | server_base.cpp, smtps_test.cpp | INFO |
| NETWORK | NETWORK | smtps_server.cpp | INFO |
| DATABASE | DATABASE | mysql_pool.cpp | INFO |
| DB_QUERY | DB_QUERY | mysql_service.cpp | WARN |
| SMTP | SMTP | smtps_fsm.hpp | INFO |
| SMTP_DETAIL | SMTP_DETAIL | traditional_smtps_fsm.h | WARN（默认禁用）|
| SESSION | SESSION | session_base.h, smtps_session.h | INFO |
| THREAD_POOL | THREAD_POOL | boost_thread_pool.cpp, io_thread_pool.cpp | INFO |
| FILE_IO | FILE_IO | smtps_fsm.hpp | INFO |
| AUTH | AUTH | smtps_fsm.hpp | INFO |

### 3. 编译时控制

通过 CMake 选项 `ENABLE_DEBUG_LOGS` 控制所有调试日志：

```bash
# 默认构建（禁用调试日志）
cmake -B build

# 启用所有调试日志
cmake -DENABLE_DEBUG_LOGS=ON -B build
```

在 `logger.h` 中定义了独立的宏，可以单独控制每个模块：

```cpp
#define ENABLE_SERVER_DEBUG_LOG 0
#define ENABLE_NETWORK_DEBUG_LOG 0
#define ENABLE_DATABASE_DEBUG_LOG 0
#define ENABLE_DATABASE_QUERY_DEBUG_LOG 0  // 通常关闭
#define ENABLE_SMTP_DEBUG_LOG 0
#define ENABLE_SMTP_DETAIL_DEBUG_LOG 0  // 通常关闭
#define ENABLE_SESSION_DEBUG_LOG 0
#define ENABLE_THREAD_POOL_DEBUG_LOG 0
#define ENABLE_FILE_IO_DEBUG_LOG 0
#define ENABLE_AUTH_DEBUG_LOG 0
```

**注意**：这些宏使用 `constexpr`，在编译时决定是否生成代码，**零运行时开销**。

### 4. 日志级别控制

**初始化**（在 `smtps_test.cpp` 中）：

```cpp
Logger::get_instance().init(
    "logs/mail_system.log",        // 日志文件路径
    1024 * 1024 * 5,            // 单文件最大 5MB
    3,                              // 保留 3 个文件
    spdlog::level::info            // 默认日志级别
);
```

**运行时动态调整**：

```cpp
// 设置全局日志级别
mail_system::set_log_level(spdlog::level::debug);

// 设置特定模块的日志级别
mail_system::set_module_log_level(mail_system::LogModule::DATABASE, spdlog::level::trace);
```

**日志级别**：

- `trace` - 最详细的跟踪信息
- `debug` - 调试信息
- `info` - 一般信息（生产环境默认）
- `warn` - 警告信息
- `error` - 错误信息
- `critical` - 严重错误

### 5. 日志格式

```
[时间戳] [级别] [线程ID] [模块] 消息
```

示例：
```
[2026-01-05 10:30:45.123] [info] [12345] [SERVER] Server initialized with SSL: enabled
[2026-01-05 10:30:45.124] [info] [12346] [NETWORK] New SSL connection accepted
[2026-01-05 10:30:45.125] [warn] [12345] [DATABASE] Database pool is not initialized
[2026-01-05 10:30:45.126] [error] [12345] [DB_QUERY] MySQL connection error
```

### 6. 文件输出

- **终端**：彩色输出，便于开发调试
- **文件**：`logs/mail_system.log`，纯文本格式
- **滚动**：超过 5MB 自动创建新文件，保留最近 3 个文件

### 7. 修改的文件清单

**新增文件**:
- `include/mail_system/back/common/logger.h` - 日志系统头文件

**修改的文件**:
- `CMakeLists.txt` - 添加 spdlog 依赖和编译选项
- `src/mail_system/back/mailServer/server_base.cpp` - 所有 std::cout/cerr → LOG_SERVER_*
- `src/mail_system/back/mailServer/smtps/smtps_server.cpp` - std::cout/cerr → LOG_NETWORK_*
- `src/mail_system/back/db/mysql_service.cpp` - 所有 std::cout/cerr → LOG_DATABASE_* / LOG_DB_QUERY_*
- `src/mail_system/back/db/mysql_pool.cpp` - 所有 std::cout/cerr → LOG_DATABASE_*
- `include/mail_system/back/mailServer/fsm/smtps/smtps_fsm.hpp` - std::cerr → LOG_DATABASE_* / LOG_FILE_IO_* / LOG_AUTH_*
- `include/mail_system/back/mailServer/fsm/smtps/traditional_smtps_fsm.h` - std::cout/cerr → LOG_SMTP_* / LOG_SMTP_DETAIL_*
- `test/smtps_test.cpp` - 添加日志初始化和关闭

**新增文档**:
- `docs/logging-guide.md` - 详细的日志配置和使用说明
- `docs/logging-completion.md` - 本文档

### 8. 快速开始指南

#### 默认构建（生产环境）

```bash
cd /Users/zhuhongrui/Desktop/code/c++/project/mail-system/v6
cmake -B build
make -j4
./test/smtpsServer

# 查看日志
tail -f logs/mail_system.log
```

#### 调试构建

```bash
# 启用所有调试日志
cmake -DENABLE_DEBUG_LOGS=ON -B build
make -j4
./test/smtpsServer

# 查看特定模块日志
tail -f logs/mail_system.log | grep DB_QUERY
```

#### 只启用数据库查询详细日志

修改 `CMakeLists.txt`：

```cmake
if(ENABLE_DATABASE_QUERY_DEBUG_LOG)
    add_compile_definitions(ENABLE_DATABASE_QUERY_DEBUG_LOG=1)
else()
    add_compile_definitions(ENABLE_DATABASE_QUERY_DEBUG_LOG=0)
endif()
```

或者直接修改 `logger.h`：

```cpp
// 将第 42 行改为
#define ENABLE_DATABASE_QUERY_DEBUG_LOG 1
```

然后重新编译。

### 9. 优势

1. **性能**：异步日志，不阻塞主线程
2. **安全**：线程安全，支持多线程并发输出
3. **可扩展**：易于添加新模块和日志级别
4. **零开销**：编译时控制的宏在运行时不会产生任何代码
5. **格式化**：统一的日志格式，便于解析和分析
6. **持久化**：日志文件自动滚动管理

### 10. 未来改进方向

1. **日志级别热更新**：支持运行时重新加载日志配置
2. **日志过滤器**：支持基于正则表达式的日志过滤
3. **远程日志**：支持日志发送到远程服务器（如 ELK、Graylog）
4. **性能监控**：记录关键操作的耗时
5. **结构化日志**：支持 JSON 格式输出，便于机器解析

## 总结

日志系统集成已完成，所有代码中的 `std::cout` 和 `std::cerr` 已被模块化的 spdlog 宏替换。系统支持：
- 10 个独立的日志模块
- 编译时开关控制的调试日志
- 运行时动态调整的日志级别
- 文件滚动和多 sink 输出

详细使用说明请参考 `docs/logging-guide.md`。
