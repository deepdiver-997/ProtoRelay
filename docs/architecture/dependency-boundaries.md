# 依赖边界与扩展范围

当前实现通过接口划分框架、邮件应用与具体后端，保持 Asio、MySQL/MariaDB 和存储后端可用。

## 已建立的边界

- `pr::ServerBase`、`TcpServerBase`、HTTP/H2 只接收框架配置和运行时。邮件配置、路由、缓存、DNS 与邮件指标由 `mail_system::MailServerBase` 承载。
- 邮件配置位于 `mail_system/back/server_config.h`；旧 `framework/server_config.h` 仅保留应用兼容入口，框架实现不得引用它取得邮件类型。
- SMTP 客户端通过 `IClientTransport`、`IExecutor`、`ITimer` 工作。CA 与主机名是配置数据，Asio socket、resolver、SSL context 只存在于后端适配器。
- 认证由可注入的 `IAccountAuthenticator` 统一承担；SMTP/Webmail 的 `AccountPolicy` 与 IMAP/POP3 委托同一实现，保留缓存、Worker 密码验证、fresh 重读、CAS rehash 和 last-login 顺序。
- 账户持久化由 `IAccountRepository` 提供，认证上下文在初读、密码 CAS、fresh 重读与 last-login 链中持有同一连接。认证服务保留密码验证和缓存策略，配额仓储封装原子更新及同连接计数，submission 复用账户查询。
- POP3 邮箱和分布式锁分别通过窄仓储操作，协议保留快照、DELE 标记、Worker 与定时器。已知写入失败停止后续写入，QUIT 在删除及释放锁完成后回到所属 executor 响应。
- IMAP 常用邮箱读取由 `IMailboxRepository` 返回业务 DTO，SQL 与列映射在实现中。连接租约继续贯穿既有协议/写操作链，UTF-7 解码留在协议适配器。
- IMAP 消息写操作由 `IImapMutationRepository` 承担，STORE 属性链、COPY/MOVE 的复制计数及源标删、EXPUNGE 使用明确请求/结果。协议层保留序号解析、响应和缓存失效，最终写操作续作回到 session executor；写失败返回 NO，不提前报告属性或删除成功。
- IMAP APPEND 通过独立服务编排 Worker 正文存储、元数据仓储及失败补偿；FETCH 元数据与文件夹操作分别使用消息、文件夹仓储，协议模板不直接构造 SQL 或解释数据库结果。
- Webmail 操作和会话分别使用 `IWebmailActionRepository`、`IWebSessionRepository`；仓储保留授权校验、事务和 fresh 凭据验证，服务负责 Worker 调度、下载流和内存会话。事务回滚失败会断开连接，避免复用未结束事务。
- Webmail 通过 `IWebmailReadRepository` 获取邮箱列表、分页和邮件元数据，`MailService` 保留分页防御、存储和 MIME 编排。SQL 查询链同时保活池与连接，元数据和正文回调不借用服务对象生命周期。
- MariaDB 通过 `IReadinessWaiter` 注入 socket 就绪等待；Asio 与阻塞 poll 是独立适配器，驱动不再探测线程局部 IO context。同步驱动可显式选用 Worker/完成 executor 装饰器。
- 持久化队列通过 `IPersistenceRepository` 完成邮件元数据、附件、Sent、去重和 outbox 的数据库操作。队列保留所有权、背压、ticket、取消和关闭；事务结果与跨分片结果分开，主提交成功后不被后续错误降级。具体 Boost lockfree 队列留在私有实现，公共头不包含 Boost 或 MySQL SDK。
- 通用存储只处理对象 IO 与通用 key 定位；邮件正文/附件布局、文件名处理、序号和存储 JSON 配置由应用层拥有。各应用存储适配器保留旧 key，异步适配器共享内层布局及计数。
- MIME 解析与元数据缓存分开。仅明确提供本地缓存能力的存储后端写本地 sidecar，其他对象 key 不作为文件路径解释。

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
| `protorelay_storage` | 通用存储实现 |
| `protorelay_mail_storage` | 邮件对象布局与存储应用适配，不依赖数据库或网络运行时 |
| `protorelay_db_mysql` / `protorelay_db_mariadb` | 数据库驱动 |
| `protorelay_smtp_client` | 可注入传输的 SMTP 客户端 |
| `protorelay_mail_core` / `protorelay_mail_protocols` | 邮件应用与协议 |

设置 `PROTORELAY_BUILD_MAIL=OFF` 后仅构建通用 HTTP/H2 与框架测试，不查找 MySQL/MariaDB、c-ares 或 CURL。`PROTORELAY_BUILD_BACKEND_CONTRACTS_ONLY=ON` 继续提供不查找 Boost/OpenSSL 的独立框架契约验证。

`COMMON_SRCS` 只为既有 object-only 打包布局保留；正常主机构建使用上述模块。

## 验证入口

一律通过 `./build.sh Release` 构建。脚本构建所有服务和注册的测试目标，并保留全局构建锁及默认并行度。

`bash test/run_all_tests.sh --no-build` 以 CTest 注册清单为准逐项运行，未构建目标会失败；数据库环境缺失以跳过状态报告。`BUILD_DIR` 可选择独立构建目录，日志默认在该目录的 `test-logs/`。

单测使用进程独占临时目录；设置 `AGENT_TMP_ROOT` 可选择目录所在位置。仓库关于串行集成测试和独立测试库的规则继续适用。

依赖方向由 `dependency_boundaries` 检查：框架不得包含邮件应用头，客户端及仓储公共 API 不得暴露具体后端类型，已迁移的业务服务和协议不得直接执行 SQL。独立后端契约测试同时验证替代实现和行为。

## 保留的实现与事务边界

现有协议、认证、Webmail、submission 和持久化队列通过接口访问领域仓储；具体 SQL、列映射、数据库 SDK、网络运行时和对象后端留在各自适配器或装配根。

- 默认邮件服务继续使用 Asio、MySQL/MariaDB、c-ares；队列私有实现继续使用 Boost lockfree。隔离公共依赖不要求替换这些实现。
- 业务仓储保留当前 MySQL SQL 方言。SQLite、原生 socket runtime 或新的对象布局属于新增能力，需要另外定义和验证。
- 正文对象和数据库保持既有跨系统非原子关系；APPEND 的失败补偿尽力执行，并报告清理是否完整。文件夹及 POP3 多步写入不新增事务或锁 fencing。
- 持久化主事务与收件人分片事务独立。主提交后的跨分片失败记录为后续处理结果，不撤销主提交，也不能删除已提交邮件正文。
- 回滚失败的连接断开后再归池；延迟回调的连接租约独立保活所属池，避免依赖服务或协议对象的销毁顺序。

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

## 并发工作原则

先约定接口与不变量，再按文件归属拆分。共享 CMake、依赖守卫和架构说明由集成者维护；协议或存储接口迁移需要指定唯一文件负责人。子任务可并行编辑和提交，统一集成后构建并执行全套回归。

构建通过全局锁串行执行，集成测试按仓库规定一次一个工作区。日常重构使用 macOS 本地构建；Docker/Linux 构建通常用于部署准备阶段，按需检查平台差异和头文件依赖。
