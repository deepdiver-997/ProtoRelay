# ProtoRelay

ProtoRelay 是基于 C++17 的邮件系统，已实现 SMTP/SMTPS、IMAP、POP3、异步外投，以及 HTTP API 和 React Webmail 前端。

## 框架 / 应用分离

代码库按可复用框架 + 应用分层组织：

```
include/framework/          # 可复用框架（与邮件协议无关）
├── server_base.h           #   服务器生命周期、配置、指标；外部注入资源
├── tcp_server_base.h       #   TCP/SSL 多协议监听器模板
├── session_base.h          #   异步读写 + FSM 集成 + 超时管理
├── fsm_base.h              #   通用状态机 (std::map dispatch)
├── fast_fsm_base.h         #   编译期数组 O(1) dispatch 变体
├── connection/             #   IConnection 抽象 + TCP/SSL 实现
├── db/                     #   DB/连接池抽象接口、配置和 RAII 租约
├── storage/                #   存储 provider、读写流及有界下载流
├── cache/                  #   通用 LruCache
├── http/                   #   HTTP/1.1 与 HTTP/2 组件
├── thread_pool/            #   IO + Worker 线程池
├── metrics_server.h        #   嵌入式 Prometheus 指标
└── intrusion_detector.h    #   基于 IP 的失败尝试跟踪与封禁

include/mail_system/back/   # 邮件系统（框架的成熟应用）
├── mailServer/             #   SMTP/IMAP/POP3 FSM + outbound 调度
├── db/                     #   MySQL/MariaDB 驱动、具体连接池和邮件 SQL
├── storage/                #   通用存储接口/实现的兼容 shim
├── smtp_client/            #   Outbound/兼容提交器共用 SMTP 客户端内核
├── persist_storage/        #   持久化队列 + Bloom 去重
├── inbound/                #   SPF/DKIM/DMARC 入站验证
└── ...
```

邮件系统目前是框架上最成熟的应用。框架组件可在其他协议服务中复用。

## 当前已实现范围

已实现认证提交、MIME/附件收取与存储、IMAP/POP3 邮箱访问、事务持久化和 outbox 外投。
各协议仍有功能边界，具体以对应协议设计为准，不声称完整 RFC 覆盖。
Outbound 嵌在 `smtpsServer` 和默认 DB 模式的 `webmailServer` 内，由数据库租约协调；
没有独立 outbound 二进制。出站边缘服务器运行自己的 SMTP 进程。

`webmailServer` 已支持共享 DB 单会话（新登录踢旧会话）、邮箱/邮件分页、MIME 读信、
通过 MailSubmissionService 直接持久化写信、已读/标星、local/distributed 流式附件下载和
原子保存已发送副本。React SPA 支持桌面三栏与手机布局、富文本 To/Cc 写信和失效草稿保留。
附件上传、服务器草稿、删除/移动和全库搜索尚未实现。
构建/运行见 [前端 README](webmail/README.md)，接口与迁移见
[Webmail API](docs/mail-system/architecture/webmail-api.md)。

## 可扩展性设计

ProtoRelay 按模块抽象构建，而不是把逻辑耦合在单体里：

- 数据库连接池抽象（`mysql`、`null` 压测模式）
- 存储适配器抽象（`local`、`null`、`s3`/MinIO、`distributed`、`hdfs_web`）
- 出站投递与 DNS 路由模块
- 配置驱动的启动装配

因此新增 provider/策略时，通常不需要改 SMTP FSM 核心路径。

## 命令行风格（向大项目靠拢）

当前 CLI 约定：

- `--help` / `-h`：统一帮助输出
- `--version` / `-V`：输出构建时注入的版本信息
- `--config` / `-c <path>`：显式指定配置文件
- 保留兼容：单个位置参数 `config_path`

示例：

```bash
./build/smtpsServer --help
./build/smtpsServer --version
./build/smtpsServer -c config/smtpsConfig.json
```

## 入站 ACK 与持久化调优

现在入站 SMTP 的确认时机可以在运行时配置：

- `inbound_ack_mode=after_persist`：只有持久化成功后才回复 `250 OK`
- `inbound_ack_mode=after_enqueue`：消息成功进入持久化队列后立即回复 `250 OK`

相关调优字段：

- `inbound_persist_wait_timeout_ms`：`after_persist` 模式下等待持久化完成的最长时间
- `persist_max_inflight_mails`：持久化链路中在途邮件总数上限
- `persist_min_available_memory_mb`：可用内存低于阈值时拒绝入队
- `persist_min_db_available_connections`：数据库连接池空闲连接过少时拒绝入队

示例：

```json
{
  "inbound_ack_mode": "after_enqueue",
  "inbound_persist_wait_timeout_ms": 5000,
  "persist_max_inflight_mails": 2048,
  "persist_min_available_memory_mb": 256,
  "persist_min_db_available_connections": 1
}
```

运维建议：

- `after_enqueue` 更适合追求吞吐和低尾延迟的场景，但 `250 OK` 不再等价于“已经可靠持久化”
- `after_persist` 更保守，适合优先保证持久化语义的场景，但吞吐会受到持久化完成时延影响
- **全矩阵压测（C++ `smtp_client`）**：详见 `test/bench/bench-report.md`
  - seq+reuse（local file storage, 无 DB）：**18,278 msg/s** @ 8 线程
  - pipe+reuse（null storage + null DB）：**72,303 msg/s** @ 32 线程 — 纯 FSM+TCP 上限
  - pipe+reuse（真实磁盘 + MySQL, 重构前）：**12,502 msg/s** @ 32 线程
  - FSM mock（零 I/O, 重构后）：**15,913 msg/s** @ 4 线程 (旧: 4,127)
  - localhost per-conn 受临时端口池限制（~16384 个）；`--local-ips` 可绕过
- M2 Pro (12 核) macOS 单机测试结果，不等同于生产 SLA
- **72303 msg/s 不含磁盘/数据库开销**（null storage + null DB），仅 FSM + TCP loopback
- 压测天花板配置：`"storage": {"provider": "null"}` + `"use_database": false`
- C++ `smtp_client` 是主要压测工具；`test/scripts/cl.py` 保留用于 TLS/AUTH 冒烟测试

### 性能调优要点

1. **IO 线程池分发**：TCP socket 必须绑定到 `IOThreadPool::get_io_context()`（轮询分发到 4 个 io_context），而非 `ServerBase::get_io_context()`（单 listener context）。曾因代码变更退化到单线程处理所有 TCP 连接，吞吐下降 7 倍。
2. **连接复用 vs 每封独立连接**：压测脚本默认复用连接（100 连接发 10000 封），更符合 benchmark 惯例。`--per-conn` 切换为每封新连接。
3. **认证缓存**：SmtpsFsm 内置 `LruCache`（TTL 5min，容量 10000），同一用户重复认证不查 DB。
4. **持久化队列**：有界 `boost::lockfree::queue`，入队/关机门闩和空闲条件变量等待；终态订阅替代 worker sleep 轮询。
5. **日志级别**：INFO 级下 spdlog 同步写 stdout 在高并发确有开销（每封 5+ 条日志抢同一把锁），压测仍建议 `warn`。但它不是吞吐天花板——DB 路径真正的热点是"TLS+阻塞 fd 使异步 DB 引擎退化成 io 线程同步执行"和"CPS 回调漏持连接导致 op 在飞归池"，均已于 2026-09-24 修复，详见 `test/bench/imap/REPORT.md` 第 6 节。
6. **SMTP 命令流水线**：`do_async_read` 入口检查命令缓冲区，如有完整行优先解析（每轮 FSM 一个命令），全部处理完后再发起网络读。不完整命令自动等待下次 TCP 到达拼接。

## 当前出站热投递语义

当前持久化队列会在同一事务中写入 `mail_outbox`，而不是等持久化完成后再单独补写。

- 如果本机可直接接手外投，会把对应 `mail_outbox` 记录以本机短租约的 `SENDING` 状态写入
- 事务提交后再把 `unique_ptr<mail>` 和这批已保留的 outbox 记录交给本地 outbound client
- 如果本地接管失败，会释放租约回到 `PENDING`，后续由其他节点继续竞争

当前这条热路径主要优化的是“本机先拿到投递权”和“少一次 claim 竞争”；正文构造仍然默认从 `body_path` 读取，所以还不算完全的纯内存外投。

## 构建时版本注入

版本信息在 CMake 配置阶段自动生成并注入二进制，包含：

- 语义化版本号
- Git 短提交号
- UTC 构建时间
- 构建目标（OS-ARCH）
- 编译器信息
- 关键特性开关

## 编译

```bash
./build.sh Debug
./build.sh Release
```

脚本会自动创建 `build/`，并尽量避免在源码根目录产生构建垃圾文件。

## 环境依赖（摘要）

- Linux/macOS
- CMake 3.10+
- GCC 9+ / Clang 10+
- Boost、OpenSSL、MySQL client、spdlog、c-ares
- 如启用 `hdfs_web` 存储：额外需要 `libcurl`

## 项目规范文档

见：

- `docs/style/PROJECT_STYLE.md`

## 写给学弟学妹

如果你正在找课程项目参考——代码随便看、随便改、随便用，不用问我。如果觉得有帮助，希望能到 GitHub 给我点个 Star，不仅是对我劳动的认可，也能让我知道这个项目帮到了人。

## 许可证

MIT，详见 `LICENSE` 文件；Boost 许可证见 `COPYING_BOOST.txt`。
