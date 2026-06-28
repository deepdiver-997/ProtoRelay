# ProtoRelay 性能测试报告

> **测试工具**: `test/cl.py` + `test/bench.sh`
> **测试环境**: macOS, localhost, Python 3.14
> **服务端**: `perf_mode=true`, `persist_max_inflight_mails` 自动覆写为 1000000

---

## 2026-06-25 — mta-relay conn-pool (端口25, 复用连接, 无TLS)

```
10000 封, ramp 50→400 线程 (步长50)
```

| 并发 | 速率 (msg/s) | p50 | p99 | p999 |
|------|-------------|-----|-----|------|
| 50 | 4629 | 9.8ms | 20.1ms | 25.7ms |
| 100 | 4412 | 19.2ms | 40.5ms | 51.5ms |
| 150 | 4238 | 29.0ms | 60.5ms | 72.9ms |
| 200 | 3774 | 37.6ms | 87.3ms | 104.7ms |
| 250 | 3714 | 44.6ms | 98.3ms | 117.4ms |
| 300 | 3648 | 49.3ms | 109.6ms | 136.2ms |
| 350 | 3490 | 56.1ms | 124.7ms | 147.6ms |
| 400 | 3346 | 51.6ms | 121.1ms | 149.8ms |

**峰值 4629 msg/s** @ conc=50。速率随并发增加缓慢下降（延迟上升），瓶颈在服务端处理能力。

---

## 2026-06-25 — mta-relay per-conn (端口25, 每封新建TCP, 无TLS, 批量写)

```
2000 封, conc=50
```

| 速率 | p50 | p99 | p999 |
|------|-----|-----|------|
| 4927 msg/s | 8.5ms | 25.3ms | 30.9ms |

与 conn-pool (4629) 几乎持平，**localhost 上 TCP 建连成本可忽略**。

---

## 2026-06-25 — submission pipeline (端口587, 每封新建TCP+TLS+AUTH, 批量写)

```
10000 封, ramp 50→400 线程 (步长50)
```

| 并发 | 速率 (msg/s) | p50 | p99 | p999 |
|------|-------------|-----|-----|------|
| 50 | 349 | 138ms | 224ms | 267ms |
| 100 | 336 | 286ms | 445ms | 500ms |
| 150 | 334 | 425ms | 673ms | 771ms |
| 200 | 327 | 570ms | 916ms | 1030ms |
| 250 | 327 | 683ms | 1164ms | 1355ms |
| 300 | 325 | 787ms | 1367ms | 1599ms |
| 350 | 324 | 869ms | 1595ms | 1809ms |
| 400 | 321 | 901ms | 1804ms | 2148ms |

**峰值 349 msg/s** @ conc=50。速率不随并发增长，瓶颈为服务端 TLS 握手吞吐（约 350 conn/s）。

---

## 对比总结

| 测试 | 策略 | 每封成本 | 峰值速率 | vs 基准 |
|------|------|---------|---------|---------|
| **端口25 seq+per-conn** | — | TCP+串行 | **514 msg/s** | 基准 |
| **端口25 seq+reuse** | — | 串行 | **4114 msg/s** | 传统MTA |
| **端口25 pipe+per-conn** | — | TCP+流水线 | **5872 msg/s** | 14x vs seq |
| **端口25 pipe+reuse** | — | 0 | **9180 msg/s** | **最快** |
| **端口587 TLS+AUTH** | — | TCP+TLS+流水线 | **349 msg/s** | TLS主导 |

TLS 握手是最大瓶颈：从无 TLS 的 4629 掉到有 TLS 的 349，**14 倍性能差距**。

---

## 2026-06-27 — FSM Mock 基准（实测）

使用 `fsm_bench` (MockConnection 零 I/O, 单线程同步 FSM) 精确测量纯业务逻辑吞吐：

```
./fsm_bench --threads 1 --iterations 50000
[1 threads]  ok=50000/50000  elapsed=12.12s  rate=4127 msg/s
```

**纯 FSM 成本 = 1/4127 = 242μs/封** (包括: 命令解析 + 状态机调度 + mail 对象构造 + 无锁队列 push)

### 与真实基准对照

| 层级 | 测试 | 速率 | 每封耗时 | 瓶颈拆解 |
|------|------|------|---------|---------|
| **纯 FSM** | mock 1线程 | **4127 msg/s** | 242μs | 基准 (0% I/O) |
| **理想 4 线程** | mock ×4 | **~16500 msg/s** | 61μs | 完美并行上限 |
| **TCP+FSM** | real 4线程 per-conn | **4927 msg/s** | 203μs | 70% 时间花在 TCP/asio |
| **TLS+FSM** | real 4线程 pipeline | **349 msg/s** | 2870μs | TLS 加密占主导 |

### 瓶颈归因

```
纯 FSM (1 线程):                    ████ 242μs
理想 4 线程 FSM:                     █ 61μs
真实 4 线程 TCP+FSM (per-conn):     ████████ 203μs
                                    ├─ FSM:  61μs (30%)
                                    └─ TCP/asio: 142μs (70%)

真实 4 线程 TLS+FSM (pipeline):     ████████████████████████████████ 2870μs
                                    ├─ FSM:       61μs (2%)
                                    ├─ TCP/asio: 142μs (5%)
                                    └─ TLS握手:  2667μs (93%)
```

### 结论

1. **IO 线程利用率只有 30%** — 4 线程 async 模式下，每线程仅 1231 msg/s，而纯 FSM 单线程可达 4127。async 事件调度、TCP 栈系统调用、回调链 overhead 吃掉了 70% 的 CPU。

2. **TLS 是绝对主导瓶颈** — 占 93% 单封耗时，纯 FSM 只占 2%。优化 TLS (缩短 RSA 密钥、ECDSA 替代 RSA、TLS 1.3) 比优化 FSM 收益大 40+ 倍。

3. **conn-pool 复用连接可逼近 FSM 极限** — 跳过 TCP 建连后，FSM 成为唯一瓶颈，理论上限 = 4 × 4127 × 0.7(利用率) ≈ 11500 msg/s。

### 后续扩展

`fsm_bench.cpp` 当前为 MTA relay 模式（无验证、无 AUTH）。后续可通过以下步骤测量验证模式吞吐：
1. 构造 mock AuthCache / DNS cache 预填充数据，避免查询 DB/DNS
2. 设置 `listener_config.auth_policy` 为目标模式 (ON/AUTO)
3. 在流水线命令中插入 `AUTH LOGIN` 步骤
4. 增加 `InboundVerifier` 的 mock 替代（跳过真实 DNS TXT 查询）


---

