# ProtoRelay

ProtoRelay 是一个基于 C++20 的邮件中继核心，当前聚焦在 SMTP 协议执行与投递链路基础能力。

## 当前已实现范围

目前项目有意保持边界清晰，重点完成 SMTP 核心三件事：

- SMTP 状态机（会话生命周期与命令流转）
- SMTP 解析（命令、信封、正文处理）
- 投递链路（队列与外发 relay 主路径）

这是一种分阶段策略：先把 relay 核心打牢，再逐步扩展更多协议与能力。

## 可扩展性设计

ProtoRelay 按模块抽象构建，而不是把逻辑耦合在单体里：

- 数据库连接池抽象（`mysql`、`mysql_distributed`）
- 存储适配器抽象（`local`、`distributed`、`hdfs_web`）
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
./test/smtpsServer --help
./test/smtpsServer --version
./test/smtpsServer --config config/smtpsConfig.json
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
- 本地压测备注：在这台 MacBook Pro 上，`after_enqueue` 小邮件测试用 `uv run ./test/cl.py` 可达到约 1.9k-3.0k msg/s；其中 1000 封约 2976.9 msg/s，10000 封、`--concurrency 100` 约 2147.5 msg/s。
- 这些数据是单机当前测试负载下的吞吐结果，不等同于生产 SLA。

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

- `docs/PROJECT_STYLE.md`

## 许可证

MIT；Boost 许可证见 `COPYING_BOOST.txt`。
