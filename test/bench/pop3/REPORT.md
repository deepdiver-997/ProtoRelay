# POP3 读路径基准报告

> 工具：`test/bench/pop3/pop3_client.cpp`（raw socket，STAT+LIST+RETR 循环）、
> `test/bench/pop3/profile.sh`（高负载采样找热点）。POP3 与 IMAP 共用同一份
> mailbox 数据（`mail_mailbox` 表），seed 复用 `test/bench/imap/seed_imap_data.py`。

## ⚠ POP3 的并发约束：单会话锁

POP3 **每用户同时只允许一个会话**（`pop3_session_lock` 表，异会话抢锁返回
`[IN-USE] Mailbox lock busy`）。因此：

- **并发连接数 ≤ 用户数**，否则后连的会话抢锁失败。压测必须用 `--users` 给每条连接
  配不同用户（`pop3_client` 默认 `--users` 轮询）。
- 被 kill 的会话会留锁约 **5 分钟**（心跳清扫阈值），快速重跑前先清：
  `mysql -e "DELETE FROM pop3_session_lock"`。

## 运行

```bash
# 构建 + 灌数据（POP3 与 IMAP 共用 mailbox；给多用户灌测数据绕过锁）
cmake --build build --target pop3_client -j 4
python3 test/bench/imap/seed_imap_data.py --mails 200 --mailbox 6
python3 test/bench/imap/seed_imap_data.py --user t1@scut.email --mailbox 11 --mails 50

# 起 pop3Server（DB-backed bench 配置，端口 1110 避开 <1024 需 root）
build/pop3Server -c /tmp/pop3_bench_config.json

# 吞吐（多用户并发绕过单会话锁）
USERS="test2@scut.email,t1@scut.email,test@scut.email,alice@a.local,bob@b.local"
./build/pop3_client --port 1110 --users "$USERS" --t 4 --conns 1 --rounds 50 --mails 5

# 高负载采样找热点（append 进本文件）
./test/bench/pop3/profile.sh --config /tmp/pop3_bench_config.json \
    --load-args "--port 1110 --users \"$USERS\" --t 4 --conns 1 --rounds 500 --mails 5"
```

## 吞吐基准（2026-08-29，5 用户，debug 构建）

> 每轮 = STAT + LIST + RETR 1:5。并发 ≤ 用户数时随并发涨；超出即抢锁失败。

| 并发（=用户数） | rounds/s | P50 (ms) | P95 (ms) | P99 (ms) |
|---------------|---------|----------|----------|----------|
| 4 | 260 | 1.27 | 2.16 | 3.09 |

## Profile 热点

_（由 `profile.sh` 自动 append）_


## POP3 Profile 热点（2026-08-29 19:54，Darwin，采样 10s，负载: --port 1110 --users test2@scut.email,t1@scut.email,test@scut.email,alice@a.local,bob@b.local,local_test@scut.email --t 6 --conns 1 --rounds 4000 --mails 10）



| 采样计数 | 热点函数 |
|---------|---------|
| 40921 | `__psynch_cvwait` |
| 12652 | `kevent` |
| 8291 | `__semwait_signal` |
| 8291 | `__sigwait` |
| 7312 | `__psynch_mutexwait` |
| 2801 | `__sendto` |
| 1843 | `__psynch_mutexdrop` |
| 1407 | `__open_nocancel` |
| 1320 | `__recvfrom` |
| 928 | `__psynch_cvsignal` |
| 720 | `std::basic_string<char>::push_back(char)` |
| 451 | `__read_nocancel` |
| 332 | `std::basic_string<char>::__init_with_sentinel[abi:ne200100]<std::istreambuf_iterator<char>, std::istreambuf_iterator<char>>(std::istreambuf_iterator<char>, std::istreambuf_iterator<char>)` |
| 238 | `__lseek` |
| 176 | `boost::asio::detail::scheduler::do_run_one(boost::asio::detail::conditionally_enabled_mutex::scoped_lock&, boost::asio::detail::scheduler_thread_info&, boost::system::error_code const&)` |
| 150 | `__close_nocancel` |
| 128 | `_platform_memmove` |
| 125 | `fstat` |
| 125 | `_nanov2_free` |
| 119 | `<deduplicated_symbol>` |


**热点解读（6 连接 × 6 用户，~2400 rounds/s，采样 10s）**：

- `__psynch_cvwait` 40.9k + `kevent` 12.7k：io 线程在 asio reactor **空闲等待**——
  POP3 读路径服务端有大量余量，吞吐是**客户端往返节奏主导**（命令多而轻）。
- `__psynch_mutexwait` 7.3k：asio scheduler 内部锁（`do_run_one` 的
  `conditionally_enabled_mutex`），并发调度成本，非应用层锁。
- `__open_nocancel` 1.4k + `__read_nocancel` 451 + `__lseek` 238 + `fstat` 125：
  **RETR 正文的 storage 文件 I/O**（每条 RETR open + read body 文件）。这是
  POP3 读路径的主要应用层热点——正文越多次重复读取，open/read 越显著。
- `std::string::push_back` / `istreambuf` init 720/332：响应拼装。

**结论**：POP3 读路径瓶颈不在服务端 CPU——io 线程空闲为主、应用热点是 RETR 的
storage 文件 I/O。若要提吞吐：少重复 RETR 同几封（读路径本来就是重复读），或把
正文读取交给 worker（远程 storage 时才有意义，本地 open/read 是 µs 级）。

## 2026-10-26 — POP3 读路径复测（Release，main@45e5663）

### 测试条件（边界）

```
构建:      Release (-O3 -DNDEBUG)
服务端:    pop3Server -c /tmp/protorelay_agent_1/pop3_bench_config.json
           20115/tcp；use_database=true → 本机 MySQL `mail_agent_1`；
           local storage（与 IMAP 共用同一份 mailbox 数据：每用户 INBOX 200 封 × 2KB）；
           4 io + 4 worker
用户:      5 个 bench 账号（test2/t1/test@scut.email、alice@a.local、bob@b.local），
           首登 rehash-to-bcrypt 已预热
每轮:      STAT + LIST + RETR 1:5（LOGIN 每连接一次）
机器:      Apple M2 Pro 12 核 / 16GB / macOS 15.8.1
```

**两条 harness 边界（影响数字解读）**：
1. **单会话锁**：并发连接数 ≤ 用户数（5），超出即 `[IN-USE] Mailbox lock busy`，
   超出部分不参与测量（本报告并发档 = 1/4/5）。
2. **pop3_client 不发 QUIT**：跑完直接关 socket，锁留待心跳清扫（~5min）。轮间必须
   `DELETE FROM pop3_session_lock`（agent 库）+ 1s 间隔，否则后续轮全部 IN-USE。
   真实客户端发 QUIT 不受影响；这是压测工具的怪癖，不是服务端缺陷。

### 吞吐

| 并发（=用户数） | rounds/s | P50 (ms) | P95 (ms) | P99 (ms) |
|---------------|----------|----------|----------|----------|
| 1 | 1390 | 0.59 | 0.71 | 0.77 |
| 4 | 4387 | 0.79 | 0.92 | 0.98 |
| 5 | 4541 | 0.85 | 1.00 | 1.07 |
| 5（10000 轮长跑） | 4841 | 0.95 | 1.13 | 1.24 |

对照说明：2026-08-29 的 260 rps @ c4 是 **Debug 构建**，与本轮 Release 不可比。

### 热点（5 并发 × 10000 轮负载，`sample` 8s）

| 采样计数 | 热点函数 |
|---------|---------|
| 25797 | `kevent` |
| 21621 | `__psynch_cvwait` |
| 2271 | `__open_nocancel`（每轮 5 次 RETR × open 正文文件） |
| 1637/1351 | `sendto` / `recvfrom` |
| 930 | `std::string::push_back`（响应拼装） |
| 470 | `istreambuf_iterator` 构造 string（正文逐迭代器读入） |

**结论**：与上轮 profile 一致——瓶颈是事件循环 + RETR 的 open/读文件；应用侧
明确的优化线索是正文读取的 `istreambuf_iterator` 逐字符 slurp（470 采样），
换成按块 read 可消。
