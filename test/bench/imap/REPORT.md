# IMAP 读路径基线（2026-08-29）

> **目的**：DB 真异步（Phase 2，database-async-design.md）的 **before** 基线。验证"io 线程
> 同步死等 MySQL/storage"这一热点假设，并为异步改造提供对照值。
>
> **工具**：`test/bench/imap/imap_client.cpp`（C++ raw socket 多连接并发；LOGIN→SELECT→FETCH 循环）。
> `test/bench/imap/seed_imap_data.py`（灌 200 封测试邮件）。
> **环境**：macOS ARM64, localhost, 4 io + 4 worker, `achieve=mysql`（当前同步 DB 路径）,
> local storage, mailbox 200 封（每封 2KB body）。
> **每轮** = SELECT INBOX + FETCH 1:200 (FLAGS RFC822.SIZE)。读场景不产生新数据，无需清理。

## 结论：热点确认 —— io 线程同步阻塞是吞吐天花板

| 并发连接 | 吞吐 (rounds/s) | P50 延迟 (ms) | P95 (ms) | P99 (ms) |
|---------|----------------|--------------|---------|---------|
| 1 | ~300–540 | 1.8–3.3 | 2.4–3.6 | 3.1–5.3 |
| 4 | ~740–1390 | 2.8–5.1 | 4.0–7.8 | 4.6–8.4 |
| 16 | ~770–1470 | 10.8–20.8 | 13.1–24.9 | 14.1–27.6 |
| 64 | ~760–1460 | 43.8–84.0 | 47.1–92.5 | 49.2–98.5 |

（两次采样因本机负载波动区间不同，但形态一致。）

**形态**：并发 > 4（= io 线程数）后吞吐**封顶**，延迟随排队线性上涨。原因：

- 每轮 FETCH 的 `get_mailbox_mails`（1 次 DB 查询）+ 200 次 `object_size`（storage 读）都在
  io 线程上**同步内联**执行——查询期间该 io 线程无法服务任何其他连接。
- 并发在途查询上限 = io 线程数（4）；超出就排队（延迟涨、吞吐平）。
- 压测中 4 个 io 线程各 ~40% CPU，但大部分时间**阻塞在 DB/storage 系统调用上**（非计算忙），
  证实是 I/O 等待而非 CPU 瓶颈。

## 对照：SELECT-only（stats 缓存命中，近零 DB）

| 并发 | 吞吐 (rounds/s) | P50 (ms) |
|-----|----------------|---------|
| 4 | 44,663 | 0.067 |
| 16 | 91,072 | 0.127 |

SELECT 的 stats 查询走缓存后接近零成本 → **瓶颈不在 SELECT，在 FETCH 的 DB 列表查询 + storage 读**。

## 预期：Phase 2（DB 真异步）应把吞吐天花板从 ~4×单线程速率抬到连接池上限（128），
且 io 线程不再被查询期间卡死。用本表做 before/after 对照。

## Phase 2 后（2026-08-29，`achieve=mariadb` 非阻塞 async）

> 同一 200-mail 邮箱，同日同期复测。对照 `achieve=mysql`（sync）与 mariadb（async）。

| 并发 | mysql sync (rounds/s) | mariadb async (rounds/s) | P50 (ms, async) |
|------|----------------------|--------------------------|-----------------|
| 1 | 609 | 586 | 1.56 |
| 4 | 1816 | 1925 | 2.02 |
| 8 | 1910 | 1891 | 4.19 |
| 16 | 1975 | 1968 | 7.99 |

**结论**：两引擎吞吐持平、都封顶 ~1950 rps。瓶颈是每轮**非 DB 的 io 线程工作**
（200 次 storage 读 + 200 行响应组装/写回），不是 DB 阻塞；本地 DB socket 立即可读，
非阻塞查询也走内联完成（rc=0 无 wait），io 线程 CPU 并不因 async 降低。**async 的收益
在远程/慢 DB 上才显现**——socket 不就绪时 io 线程在 `async_wait` 期间真正让出。
SELECT-only（缓存命中近零 DB）两引擎均 ~40k rps。

**顺带修掉的真 bug：async op 连接泄漏**（Phase 2 实施时发现）：`done` 捕获 op 自身构成
shared_ptr 循环 → 每个 async 查询泄漏一条连接 → 池耗尽 → io 线程卡 5s。压测复现
89 acquire / 1 release。修后 acquire=release。回归单测 `mariadb_async_test`
（50 查询后池 available 恢复基线）。

## ⚠ 顺带发现的生产 bug：FETCH 续作链栈溢出

FETCH 大邮箱（>~200 封）会 **SIGSEGV**：`fetch_complete_mail_with_body` 栈溢出。
本地 storage 的 `async_object_size/read` 回调**内联**触发 → `fetch_drive` 被逐封重入（真递归），
500 封即爆栈（lldb 确认 EXC_BAD_ACCESS code=2，栈指针落在 guard 页，无法 unwind）。
`server_base.cpp:137` 的设计注释说"本地内联 µs 级"，`fetch_drive` 注释称"万封不爆栈"——两者
在内联回调下矛盾。**已修复（2026-08-29，`fetch_drive` 续作链迭代化）**：每封共享一个 `std::atomic<bool> alive`，
size/body 两个异步读共用；完成本封的最后一步走 `fetch_continue`——inline 回调（外层
`fetch_drive` 帧仍在）→ 外层循环 continue；deferred 回调 → 驱动下一封。本地 storage
保持内联（不加线程投递开销）。回归单测 `fetch_many_mails_no_stack_overflow`（400 封）+
实测 FETCH 1:2000 正常。




## Profile 热点（2026-08-29 03:03，Darwin，采样 10s，负载: --t 16 --conns 4 --rounds 2000）

> 由 `test/bench/imap/profile.sh` 生成（release 构建，mysql 引擎）。`__psynch_cvwait`/`kevent`
> 是 asio reactor 的空闲等待（正常）；`stat`=storage 读、`__recvfrom`=网络读、
> `MySQLResult::get_value`=DB 结果解析是真实应用热点。

  wall=48.4677s  rounds=128000  throughput=2640.93 rounds/s

| 采样计数 | 热点函数 |
|---------|---------|
| 33596 | `__psynch_cvwait` |
| 20189 | `__recvfrom` |
| 8445 | `kevent` |
| 8396 | `__sigwait` |
| 8396 | `__semwait_signal` |
| 8119 | `stat` |
| 550 | `_platform_memmove` |
| 441 | `__sendto` |
| 291 | `mail_system::MySQLResult::get_value(unsigned long, std::basic_string<char> const&) const` |
| 258 | `_nanov2_free` |
| 203 | `_platform_memcmp` |
| 190 | `__psynch_mutexwait` |

## Phase 3 后（2026-08-29，`achieve=mariadb` + prepared stmt 缓存 + `mysql_ping` 保活）

> 同一 200-mail 邮箱、同一 debug 构建对照。缓存命中直接复用 stmt（跳过 prepare），
> 每查询往返 2 → 1。

| 并发 | Phase 2（无缓存） | Phase 3（缓存） | P50 (ms, Phase 3) |
|------|------------------|----------------|-------------------|
| 1 | 586 | 629 | 1.51 |
| 4 | 1925 | 1967 | 1.98 |
| 8 | 1891 | 2003 | 3.94 |
| 16 | 1968 | 1987 | 7.73 |

**验证往返减半**：`SHOW GLOBAL STATUS` 实测 25,732 次 `Com_stmt_execute` 仅 10 次
`Com_stmt_prepare`（distinct SQL 数）。localhost 吞吐只涨 ~3%——本地 prepare 本就快、
轮次被 200 行结果 + storage 读主导；**对远程 DB（部署目标）才是大头**。
`mysql_ping()` 保活替代 SELECT 1（不污染缓存 stmt 状态）；功能回归
（LOGIN/SELECT/FETCH 经缓存 stmt 数据正确）、ctest 全绿、TSan 无 race。

## 异步路径 9 月修复后复测（2026-09-22，HEAD `177c1da`，Release 构建）

> **背景**：Phase 2/3 复测在 8/29 当天（Debug 构建）。此后异步路径又落了多个修复
> （stmt 截断重取 UAF、结果列 256 字节缓冲、IOThreadPool 每线程独立 io_context 等），
> 本文是修复后的首次复测。Release 构建 + `log_level=warn`（与 8/29 Debug+info 不可直比，
> 本次内部对照自洽）。4 io + 4 worker，200 封邮箱，每轮 = SELECT + FETCH 1:200 (FLAGS RFC822.SIZE)，
> 经 `SHOW GLOBAL STATUS` 验证每轮真实打 1 次 DB 查询。

### 1. 本地 DB（localhost socket，查询即达）

| 并发 | mysql sync (rps) | mariadb async (rps) | P50 async (ms) |
|------|-----------------|--------------------|----------------|
| 1    | 491             | 398                | 2.60           |
| 4    | 1682            | 1695               | 2.22           |
| 16   | 1676            | 1665               | 8.92           |
| 64   | 1709            | 1663               | 7.88           |
| 64 SELECT-only | 119,928 | 125,321          | 0.11           |

- 两引擎形态一致：c=4 起封顶 ~1700 rps，并发再高只涨延迟不涨吞吐。
- c=1 三次采样：sync 435–491 rps（P50 1.79–1.96ms），async 361–370 rps（P50 2.68–2.77ms）——
  **DB 即达时 async 单查询反而慢 ~20%**（异步机制固定开销）。
- 16 io 线程复跑 c=16/64：1815/1688 (sync)、1826/1705 (async)——**吞吐封顶不随 io 线程数变**，
  说明瓶颈不在 io 线程算力。

### 2. 慢 DB 实验（TCP 代理加 5ms/chunk 双向延迟 ≈ 每查询 10–15ms RTT，模拟远程 DB）

| 配置 | mysql sync | mariadb async |
|------|-----------|---------------|
| c=4, 4 io 线程   | 81 rps | 82 rps |
| c=16, 4 io 线程  | 80 rps | 83 rps |
| c=64, 4 io 线程  | 80 rps | 83 rps |
| c=16/64, 16 io 线程 | 68 rps | 67 rps |

- **两引擎、任何并发、任何 io 线程数都钉死在 ~67–83 rps**；线程版代理 + `Threads_running`
  采样（中位 2）排除代理与 MySQL 本身串行。
- Little 定律对账：67 rps × ~15ms ≈ 1.0 —— **任何时刻只有 ~1 个 DB 操作在飞**。
  本地封顶同样对账：1700 rps × ~0.58ms ≈ 1.0。同一个串行点，只是被本地 DB 的快
  掩盖成了"非 DB 瓶颈"的假象。
- 延迟随并发线性涨（c=4 P50 58ms → c=16 P50 210ms）、吞吐不涨 = 单队列排队，实锤。

### 3. 结论

1. **异步化改造没有引入性能回归**（本地 DB 各并发点与 sync 持平），9 月的异步路径修复未劣化。
2. **DB 访问路径存在一个两引擎共用的全局串行点**（单飞）：吞吐 = 1/单次 DB 操作耗时。
   async 的理论收益（等待期间让出 io 线程 → 查询并发，直连连接池 128）**尚未兑现**——
   慢 DB 下本应抬到连接池上限，实测仍与 sync 同值。
3. 下一步：root-cause 该串行点（嫌疑：stmt 缓存锁 / 池级锁在查询执行期间持锁；
   或 async op 链的串行化），目标是慢 DB 下吞吐随并发抬升到 min(并发, 池大小)。

> ⚠ 复测过程中的坑（他人复现时注意）：`db_config_file` 的 `initialize_script` 按**配置文件
> 所在目录**解析，配置放 /tmp 时池初始化失败、静默变为全失败错误路径（吞吐虚高 10 倍+、
> 客户端无感知）。压测前必须核对服务端日志无 `Failed to get database connection`，
> 并用 Questions 计数器验证查询真实落库。

### 4. 串行点根因定位（2026-09-22 当日，代码排查 + 计数器实证）

**根因：`MySQLPool::get_connection()`（`mysql_pool.cpp:232`）在池级全局锁 `m_mutex`
内做 checkout 校验**——第 289 行 `validate_connection()` → `connection->ping()` 是一次
**同步网络往返（COM_PING / SELECT 1）**，且 `release_connection` 也要同一把锁。
于是全池所有线程的借/还连接串成单队列，每个 DB 操作都被强加"前一个人的 ping 往返"。
两个引擎共用此处：`mariadb_pool.cpp:18` 注释明言"池是引擎无关的，直接复用 MySQLPool"。

**实证**：

- `performance_schema.events_statements_summary_global_by_event_name`
  （`statement/com/ping`）：2000 轮压测 COM_PING **+2002**、Questions +2016——
  每轮恰好 1 ping + 1 查询，ping 在每轮关键路径上。
- Little 定律两头闭合：慢 DB（ping≈15ms）→ 理论 67 rps，实测 67–83；
  本地（ping≈0.58ms）→ 理论 ~1700 rps，实测 ~1700。第 2 节的"非 DB 工作瓶颈"假象
  实为锁内 ping（本地 DB 快，把它伪装成计算瓶颈）。
- 不变性吻合：吞吐与并发（4→64）、io 线程数（4→16）全无关（全局锁性质），
  延迟线性涨（单队列排队）。SELECT-only 120k rps——stats 缓存命中不 checkout，无 ping。

**排除项**：mariadb 异步 op 机制本身是真异步 + 真多路复用（每 op 独立
`posix::stream_descriptor` + `async_wait`，事件驱动续作，fd 所有权 release 归还正确），
查询可以并发——只是每个查询进门都要先过这道串行 checkout 关卡。
c=1 时 async 比 sync 慢 ~20%（P50 2.6 vs 1.9ms）与 ping 无关，是异步路径每步在堆上
新建 descriptor/timer/atomic 的固定开销（~0.5–1ms/查询）。

**修复方向（按投入排序）**：

1. 最小改动：ping 移出锁（锁内只 pop，锁外校验），或去掉 checkout ping 改为
   "查询失败标记连接失效 + 重试一次"（标准池模式；`maintenance_thread` 已有保活）。
   预期慢 DB 吞吐抬到 min(并发, 池大小)。
2. 中期：checkout 异步化（acquire CPS 化），池耗尽时 io 线程不再同步死等 5s。
3. 顺带：per-connection 复用 stream_descriptor，削减异步固定开销。

**评估过并否决的方向**：无锁池（瓶颈是锁内网络往返而非锁本身，出锁后临界区 µs 级，
无锁的 ABA/内存回收复杂度不成比例；若真现锁竞争，按 io 线程分片池更简单有效）；
多查询合并单连接提交（MySQL 协议一连接不允许多请求在途，合并需引入结果归位/
部分失败/头阻塞等拆分复杂度，且与 Phase 3 的 prepared stmt 缓存互斥，见下）。
