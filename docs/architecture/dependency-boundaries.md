# 依赖边界与后续重构

当前实现通过接口划分框架、邮件应用与具体后端，保持 Asio、MySQL/MariaDB 和存储后端可用。

## 已建立的边界

- `pr::ServerBase`、`TcpServerBase`、HTTP/H2 只接收框架配置和运行时。邮件配置、路由、缓存、DNS 与邮件指标由 `mail_system::MailServerBase` 承载。
- 邮件配置位于 `mail_system/back/server_config.h`；旧 `framework/server_config.h` 仅保留应用兼容入口，框架实现不得引用它取得邮件类型。
- SMTP 客户端通过 `IClientTransport`、`IExecutor`、`ITimer` 工作。CA 与主机名是配置数据，Asio socket、resolver、SSL context 只存在于后端适配器。
- 认证由可注入的 `IAccountAuthenticator` 统一承担；SMTP/Webmail 的 `AccountPolicy` 与 IMAP/POP3 委托同一实现，保留缓存、Worker 密码验证、fresh 重读、CAS rehash 和 last-login 顺序。
- IMAP 常用邮箱读取由 `IMailboxRepository` 返回业务 DTO，SQL 与列映射在实现中。连接租约继续贯穿既有协议/写操作链，UTF-7 解码留在协议适配器。
- IMAP 消息写操作由 `IImapMutationRepository` 承担，STORE 属性链、COPY/MOVE 的复制计数及源标删、EXPUNGE 使用明确请求/结果。协议层保留序号解析、响应和缓存失效，最终写操作续作回到 session executor；APPEND 与文件夹管理仍待迁移。
- Webmail 通过 `IWebmailReadRepository` 获取邮箱列表、分页和邮件元数据，`MailService` 保留分页防御、存储和 MIME 编排。SQL 查询链同时保活池与连接，元数据和正文回调不借用服务对象生命周期。
- MariaDB 通过 `IReadinessWaiter` 注入 socket 就绪等待；Asio 与阻塞 poll 是独立适配器，驱动不再探测线程局部 IO context。同步驱动可显式选用 Worker/完成 executor 装饰器。
- 存储对象操作不包含本地流实现；MIME 解析与元数据缓存分开。仅明确提供本地缓存能力的存储后端写本地 sidecar，其他对象 key 不作为文件路径解释。

## 构建模块

正常 macOS/主机构建中，各模块编译为独立静态库，避免每个服务和测试重复编译整份公共源：

| 模块 | 职责 |
|---|---|
| `protorelay_logging` | 日志接口及所选输出后端 |
| `protorelay_framework` | 通用服务器、会话基类 |
| `protorelay_asio` | 服务端/客户端网络适配器 |
| `protorelay_runtime` | IO 与所选 Worker 实现 |
| `protorelay_readiness` | POSIX poll 就绪等待适配 |
| `protorelay_http` / `protorelay_h2` | HTTP/1、HTTP/2 协议 |
| `protorelay_storage` | 存储实现 |
| `protorelay_db_mysql` / `protorelay_db_mariadb` | 数据库驱动 |
| `protorelay_smtp_client` | 可注入传输的 SMTP 客户端 |
| `protorelay_mail_core` / `protorelay_mail_protocols` | 邮件应用与协议 |

设置 `PROTORELAY_BUILD_MAIL=OFF` 后仅构建通用 HTTP/H2 与框架测试，不查找 MySQL/MariaDB、c-ares 或 CURL。`PROTORELAY_BUILD_BACKEND_CONTRACTS_ONLY=ON` 继续提供不查找 Boost/OpenSSL 的独立框架契约验证。

`COMMON_SRCS` 只为既有 object-only 打包布局保留；正常主机构建使用上述模块。

## 验证入口

一律通过 `./build.sh Release` 构建。脚本构建所有服务和注册的测试目标，并保留全局构建锁及默认并行度。

`bash test/run_all_tests.sh --no-build` 以 CTest 注册清单为准逐项运行，未构建目标会失败；数据库环境缺失以跳过状态报告。`BUILD_DIR` 可选择独立构建目录，日志默认在该目录的 `test-logs/`。

单测使用进程独占临时目录；设置 `AGENT_TMP_ROOT` 可选择目录所在位置。仓库关于串行集成测试和独立测试库的规则继续适用。

依赖方向由 `dependency_boundaries` 检查：框架不得包含邮件应用头，客户端公共 API 不得包含或暴露 Boost 类型。独立后端契约测试同时验证替代实现和行为。

## 后续范围

- 将 IMAP APPEND 的落盘、事务和清理补偿，以及文件夹管理继续迁入业务仓储；避免仅将长函数机械换位置。
- 继续抽离 Webmail 写操作、会话存储及 IMAP/POP3 剩余查询，并在业务服务间复用查询语义；保持各协议的响应、排序与权限规则。
- 邮件正文/附件 key 生成接口当前保留兼容方法，后续可在所有提供者迁移后移到邮件对象布局策略。
- 原生网络运行时、SQLite、持久化队列中 Boost lockfree 的替换仍是独立工作；移除 Asio 不代表已经移除全部 Boost，也不保证 SQL 方言可移植。


## 独立构建配置

通过构建脚本指定独立目录，验证模块是否仍依赖不需要的 SDK：


```sh
BUILD_DIR=build/framework ARTIFACT_DIR=artifacts/framework PROTORELAY_LOG_BACKEND=STANDARD \
  EXTRA_CMAKE_ARGS='-DPROTORELAY_BUILD_MAIL=OFF -DCMAKE_DISABLE_FIND_PACKAGE_CURL=ON -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON' ./build.sh Release
BUILD_DIR=build/framework bash test/run_all_tests.sh --no-build

BUILD_DIR=build/contracts ARTIFACT_DIR=artifacts/contracts \
  EXTRA_CMAKE_ARGS='-DPROTORELAY_BUILD_BACKEND_CONTRACTS_ONLY=ON -DCMAKE_DISABLE_FIND_PACKAGE_Boost=ON -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON -DCMAKE_DISABLE_FIND_PACKAGE_spdlog=ON' ./build.sh Release
BUILD_DIR=build/contracts bash test/run_all_tests.sh --no-build
```

## 多 agent 拆分建议

先约定共享接口，再按文件归属拆分，避免多个 agent 同改协议模板或 CMake 主文件：

| 方向 | 可并发性与前置条件 |
|---|---|
| IMAP APPEND/文件夹仓储 | 可与 Webmail 写服务并发；先约定事务、连接租约、正文落盘及失败补偿接口 |
| Webmail 写操作/会话存储 | 可独立开始；与 IMAP/POP3 共享 DTO 和权限语义由集成者统一 |
| 对象 key 布局策略 | 可与上述两项并发；保持旧 key 可读，集中由一个 agent 修改提供者公共接口 |
| 原生网络后端 | 可基于现有连接/执行器/定时器/传输契约独立开发，复用相同契约测试 |
| SQLite 等数据库后端 | 驱动可并发验证；业务 SQL 方言适配需等仓储接口和事务语义稳定 |
| 持久化队列去 Boost lockfree | 可独立评估，需保持压力控制、持久化确认和关闭语义 |

构建仍通过全局锁串行执行，集成测试仍按仓库规定一次一个工作区；日常重构由集成分支统一跑 macOS 本地构建和全量测试；Docker/Linux 构建通常用于部署准备阶段，按需检查平台差异和头文件依赖。
