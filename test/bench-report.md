# ProtoRelay 性能测试报告

> **最后更新**: 2026-06-28
> **测试工具**: C++ `smtp_client` (raw sockets, TCP_NODELAY), 替代了之前的 Python `cl.py`
> **测试环境**: macOS ARM64, localhost
> **服务端**: `perf_mode=true`, `persist_max_inflight_mails=1000000`, `inbound_ack_mode=after_enqueue`
> **服务端线程**: 4 io + 4 worker

---

## 重要更正 (2026-06-28)

**以下早期测试结果标记为过时/可能有误**（2026-06-25 用 Python `cl.py` 测试）:

| 标记 | 原因 |
|------|------|
| ~~端口25 seq+per-conn: 514 msg/s~~ | Python smtplib 开销大, C++ 实测 **6359 msg/s** |
| ~~端口25 seq+reuse: 4114 msg/s~~ | Python smtplib 开销大, C++ 实测 **11147 msg/s** |
| ~~端口25 pipe+per-conn: 5872 msg/s~~ | 未控制 ephemeral port 耗尽, 且 Python 开销 |
| ~~端口25 pipe+reuse: 9180 msg/s~~ | 未做每轮清理, 文件堆积影响结果 |

Python `cl.py` 的结果受限于:
1. Python smtplib + 线程开销 (GIL)
2. 未在每轮间清理 mail 目录, 文件堆积拖慢后续测试
3. localhost 下 per-conn 测试未考虑 ephemeral port 耗尽（四元组冲突）

**以下 2026-06-28 C++ 结果才是可信的.**

---

## 2026-06-28 — 全投递路径矩阵 (C++ smtp_client, port 25, no TLS)

### 测试条件

```
工具:      C++ smtp_client (raw BSD sockets, TCP_NODELAY)
服务端:    smtpsServer, perf_mode=true, 4 io + 4 worker
网络:      localhost (127.0.0.1:25)
TLS:       无 (port 25 MTA relay)
per-conn:  每封邮件新建 TCP 连接 + EHLO + 事务 + QUIT
reuse:     每线程复用一条 TCP 连接, 首封 EHLO, 后续直接 MAIL FROM
pipeline:  所有 SMTP 命令一次 write() 批量发送
sequential:逐命令发送 + 等待响应 (smtplib 风格)
清理策略:  reuse 测试每轮 kill server + cleanup.sh
           per-conn 测试每个组合间等待 TIME_WAIT 排空 (35s)
消息量:    per-conn 5000 封 (受限于 localhost ephemeral port ~16384)
           reuse 50000 封 (不受端口限制)
```

### 结果

#### 1. Sequential + Per-Conn (串行命令, 每封新建 TCP)

| 线程 | 成功 | 失败 | 耗时 | 速率 |
|------|------|------|------|------|
| 1 | 5000 | 0 | 2.58s | 1940 msg/s |
| 2 | 4998 | 2 | 1.55s | 3220 msg/s |
| 4 | 4996 | 4 | 1.08s | 4621 msg/s |
| **8** | **4984** | **16** | **0.78s** | **6359 msg/s** |
| 16 | 4980 | 20 | 0.91s | 5477 msg/s |

**峰值 6359 msg/s @ t=8**

#### 2. Sequential + Reuse (串行命令, MTA 复用 TCP)

| 线程 | 成功 | 失败 | 耗时 | 速率 |
|------|------|------|------|------|
| 1 | 5000 | 0 | 1.64s | 3057 msg/s |
| 2 | 5000 | 0 | 1.16s | 4295 msg/s |
| 4 | 5000 | 0 | 0.64s | 7799 msg/s |
| 8 | 5000 | 0 | 0.55s | 9101 msg/s |
| **16** | **5000** | **0** | **0.45s** | **11147 msg/s** |
| 32 | 5000 | 0 | 0.59s | 8490 msg/s |

**峰值 11147 msg/s @ t=16**, 零失败

#### 3. Pipeline + Per-Conn (批量写, 每封新建 TCP)

| 线程 | 成功 | 失败 | 耗时 | 速率 |
|------|------|------|------|------|
| 1 | 5000 | 0 | 2.09s | 2390 msg/s |
| 2 | 5000 | 0 | 1.26s | 3975 msg/s |
| **4** | **4989** | **11** | **0.88s** | **5657 msg/s** |
| 8 | 1352 | 3648 | 1.67s | 807 msg/s |

**峰值 5657 msg/s @ t=4**

t=8 时大量失败 — pipeline 连接生命周期极短, 连接到达速率超过服务端 accept backlog (`kern.ipc.somaxconn=128`)

#### 4. Pipeline + Reuse (批量写, MTA 复用 TCP, 最大吞吐)

| 线程 | 成功 | 失败 | 耗时 | 速率 |
|------|------|------|------|------|
| 1 | 50000 | 0 | 13.40s | 3731 msg/s |
| 2 | 50000 | 0 | 7.77s | 6436 msg/s |
| 4 | 50000 | 0 | 5.45s | 9179 msg/s |
| 8 | 50000 | 0 | 4.17s | 11988 msg/s |
| 16 | 50000 | 0 | 4.19s | 11921 msg/s |
| **32** | **50000** | **0** | **4.00s** | **12502 msg/s** |
| 64 | 50000 | 0 | 4.19s | 11919 msg/s |

**峰值 12502 msg/s @ t=32**, 零失败。每轮独立重启+清理, 避免文件堆积

---

## 对比总结

| # | 投递路径 | 峰值 | 最优t | 说明 |
|---|---------|------|-------|------|
| 1 | seq + per-conn | **6,359** | 8 | 最慢, 每封 TCP握手+EHLO+逐命令往返 |
| 2 | seq + reuse | **11,147** | 16 | 传统 MTA 模式, 复用连接省 TCP 握手 |
| 3 | pipe + per-conn | **5,657** | 4 | 批量写省往返, 但 TCP 握手+backlog 限制并发 |
| 4 | pipe + reuse | **12,502** | 32 | **最快**, pipeline 省往返 + reuse 省握手 |

### 收益拆解

```
seq + per-conn                              6359 msg/s  (基准)
seq + reuse    +75%  (连接复用)             11147 msg/s
pipe + per-conn -11% (批量写但TCP握手抵消)   5657 msg/s
pipe + reuse   +97%  (连接复用+批量写)      12502 msg/s
```

**连接复用是收益最大的单项优化 (+75%)**。Pipeline 在 per-conn 场景下因 accept backlog 瓶颈反而不如 seq, 但在 reuse 场景下叠加后有额外 +12% 收益。

---

## localhost per-conn 测试的固有限制

### ephemeral port 耗尽 (四元组冲突)

```
macOS 临时端口范围: 49152 - 65535 (16384 个)
TIME_WAIT 持续:     2 × MSL = 30 秒
```

per-conn 每封邮件一个 TCP 连接, 四元组 `(127.0.0.1, client_port, 127.0.0.1, 25)` 中 client_port 来自临时端口池。

连接关闭后端口进入 TIME_WAIT (30s), **同一四元组不可复用**。`SO_REUSEADDR` 对客户端 `connect()` 不生效, 因为四元组完全相同时 TCP 协议栈会拒绝。

**30 秒窗口内 localhost per-conn 的理论上限 ≈ 16384 封邮件。** 超过此数量 connect() 必定失败 (EADDRNOTAVAIL)。

```
实测验证:
./build/smtp_client                    # pipe per-conn, 50000 msgs
pipe=Y reuse=N total=50000 ok=16350 fail=33650  ← 成功数 ≈ 临时端口数
```

### 生产环境不受影响

生产环境中 MTA 连接的是**不同外部服务器 IP**, 四元组自然不同:
```
(本机IP, port_X, mail.example.com, 25)  ≠  (本机IP, port_X, mx.other.com, 25)
```

port_X 可安全复用。此限制仅存在于 localhost 单 IP 基准测试场景。

### 规避方案

- **reuse 测试**: 无限制 (每线程一个连接, 总共 N 个四元组)
- **per-conn 测试**: msg ≤ 5000, 配合 TIME_WAIT 排空等待 (35s)
- **多 IP**: 使用 loopback 别名 (127.0.0.2 ~ 127.0.0.255) 增加四元组空间

---

## FSM Mock 基准 (2026-06-27)

使用 `fsm_bench` (MockConnection 零 I/O) 测量纯业务逻辑吞吐:

```
./fsm_bench --threads 1 --iterations 50000
ok=50000/50000  elapsed=12.12s  rate=4127 msg/s
```

**纯 FSM 成本 = 242μs/封** (命令解析 + 状态机调度 + mail 对象构造 + 队列 push)

### 与真实基准对照

| 层级 | 测试 | 峰值速率 | 每封耗时 | 瓶颈拆解 |
|------|------|---------|---------|---------|
| **纯 FSM** | mock 1线程 | 4127 msg/s | 242μs | 基准 (0% I/O) |
| **理想 4 线程** | mock ×4 | ~16500 msg/s | 61μs | 完美并行上限 |
| **pipe+reuse** | real 32线程 | 12502 msg/s | 80μs | 逼近理想上限的 76% |
| **seq+reuse** | real 16线程 | 11147 msg/s | 90μs | 逐命令往返增加延迟 |
| **seq+per-conn** | real 8线程 | 6359 msg/s | 157μs | TCP 握手主导 |

### 瓶颈归因 (pipe+reuse)

```
纯 FSM (1 线程):                      ████ 242μs
4 线程 FSM（理想）:                    █ 61μs
32 线程 pipe+reuse（真实）:            ██ 80μs
                                    ├─ FSM:       61μs (76%)
                                    └─ TCP/asio:  19μs (24%)
```

**pipe+reuse 已达 FSM 并行上限的 76%** — async I/O 框架 overhead 仅 24%, 连接复用 + 流水线几乎消除了所有网络开销。

---

## 关于 fileprovider 进程 CPU 占用

你在测试期间看到的高 CPU "fileprovider" 进程是 macOS 系统的 **`fileproviderd`** (File Provider daemon), 不是本项目代码。

**触发原因**: 你的项目路径在 `~/Desktop/` 下, 而 Desktop 默认开启 iCloud 云盘同步。基准测试在 `mail/` 和 `attachments/` 目录中短时间写入并删除数万文件:

```
pipe+reuse t=32:  50000 封 → mail/ 写入 50000 文件 → cleanup 全部删除
seq+per-conn:     5000 封  → 重复 5 轮
总写入量:         约 10+ 万文件/轮
```

`fileproviderd` 检测到这些文件变更后尝试同步到 iCloud, 导致 CPU 飙升。

**解决方案**:
1. `test/cleanup.sh` 已经在测试间清理 `mail/` 和 `attachments/`
2. 可以将项目移到非 iCloud 同步目录 (如 `~/projects/`)
3. 或在系统设置中关闭 Desktop & Documents 的 iCloud 同步
4. 将 `mail_storage_path` 配置指向 `/tmp/` 等非同步目录

这**不影响测试结果本身** — `fileproviderd` 是独立的系统进程, 不会阻塞或修改 smtpsServer 的处理逻辑。但它可能间接触发 I/O 竞争使磁盘写入延迟抖动。

---

## 2026-06-25 — 历史数据 (Python cl.py, 仅供参考)

> **以下结果已标记为过时**, 保留仅为历史记录。条件说明不完整, 工具链不同, 不可与新结果直接比较。

### ~~mta-relay conn-pool (端口25, 复用连接, 无TLS)~~

```
10000 封, ramp 50→400 并发 (步长50), Python smtplib
```

| 并发 | 速率 (msg/s) | p50 | p99 | p999 |
|------|-------------|-----|-----|------|
| 50 | 4629 | 9.8ms | 20.1ms | 25.7ms |
| ... | ... | ... | ... | ... |
| 400 | 3346 | 51.6ms | 121.1ms | 149.8ms |

~~峰值 4629 msg/s @ conc=50~~

### ~~mta-relay per-conn (端口25, 每封新建TCP, 批量写)~~

```
2000 封, conc=50, Python smtplib
```

| 速率 | p50 | p99 | p999 |
|------|-----|-----|------|
| 4927 msg/s | 8.5ms | 25.3ms | 30.9ms |

**注意**: 2000 封远小于 ephemeral port 限制, 所以未触发端口耗尽。当时结论 "localhost 上 TCP 建连成本可忽略" **不准确** — 小幅测试感知不到 TCP 握手开销, C++ 大量测试表明 TCP 建连是主要瓶颈。

### ~~submission pipeline (端口587, TLS+AUTH, 批量写)~~

~~峰值 349 msg/s @ conc=50。TLS 握手是最大瓶颈。~~

---

## 对 `smtp_client.cpp` 的修改 (2026-06-28)

原 C++ `smtp_client` 仅支持 pipeline 模式。新增能力:

- `--seq` 标志: 关闭流水线, 启用串行命令模式 (逐命令发送+等待响应)
- `--pipe` 标志: 显式启用流水线 (默认开启)
- `--reuse` 标志: 启用连接复用 (默认 per-conn)
- `SO_REUSEADDR`: 对多目标 IP 场景有效, localhost 场景无效

Worker 选择矩阵:

| `--pipe` | `--reuse` | Worker 函数 | 模拟场景 |
|----------|-----------|------------|---------|
| N (--seq) | N | `worker_seq_perconn` | 传统客户端, 不优化 |
| N (--seq) | Y (--reuse) | `worker_seq_reuse` | 传统 MTA 中继 |
| Y (default) | N | `worker_pipe_perconn` | 现代客户端批量写 |
| Y (default) | Y (--reuse) | `worker_pipe_reuse` | MTA 中继最大吞吐 |
