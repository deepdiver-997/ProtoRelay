# 性能测试（bench）索引

按协议组织：每个协议一个子目录，吞吐基准 + 采样热点分析 + 报告放一起。

```
test/bench/
  README.md                  ← 本文件
  run_bench_all.sh           跑全部压测（SMTP 优先，见文件内注释）
  smtp/
    round.sh                 标准四象限基准（读服务端配置取端口/存储；支持 ack 模式/
                             入队上限覆盖/local-ips/象限选择，全部环境变量参数化）
    smtp_client.cpp          SMTP 压测客户端（raw socket，线程×连接；--local-ips
                             每 worker 绑一个回环 IP 扩四元组空间）
    bench.sh                 SMTP 压测启动器（ramp 并发找峰值；依赖 scripts/cl.py，已失修）
    profile.sh               高负载采样找热点（写路径，跑完按需清理）
    REPORT.md                SMTP 吞吐基准报告（历史全量）
  imap/
    round.sh                 标准读路径基准（端口从配置读；预热登录/SELECT 对照可调）
    imap_client.cpp          IMAP 读路径压测客户端（SELECT+FETCH 循环）
    seed_imap_data.py        灌测试邮件（INSERT IGNORE 幂等；POP3 也共用此数据）
    profile.sh               高负载采样找热点（macOS sample / Linux perf）
    REPORT.md                IMAP 吞吐基准 + Phase 2/3 对照 + 热点报告
  pop3/
    round.sh                 标准读路径基准（端口/DB 从配置读；轮间自动清会话锁）
    pop3_client.cpp          POP3 读路径压测客户端（STAT+LIST+RETR 循环）
    profile.sh               高负载采样找热点
    REPORT.md                POP3 吞吐基准 + 热点报告
  fsm/
    fsm_bench.cpp            纯 FSM 基准（MockConnection 零 I/O）
    dispatch_bench.cpp       分发器基准
```

## round.sh 约定（推荐入口）

三个 `round.sh` 是"脚本只管跑"的标准基准：端口、存储路径、DB 连接全部从
`CONFIG` 指向的服务端配置 JSON 读取，脚本自身零硬编码资源；资源占用（线程档位、
消息量、象限选择、ack 模式、入队上限、本地 IP 列表）用环境变量覆盖。默认值对齐
各 REPORT.md 最近一轮的口径，直接跑即可复现。结果打 stdout，誊进对应 REPORT.md。

**per-conn 大并发的前置条件（仅 SMTP）**：macOS 单个回环 IP 只有 16K ephemeral
端口（TIME_WAIT 30s ≈ 546 新建连接/秒/IP），端口池耗尽会伪装成服务端崩塌。跑
象限 1/3 前挂一次性别名（重启失效，不持久化）：

```bash
for i in $(seq 2 17); do sudo ifconfig lo0 alias 127.0.0.$i 255.255.255.255 up; done
```

然后 `LOCAL_IPS=127.0.0.2,127.0.0.3,... bash test/bench/smtp/round.sh`。round.sh
预检 LOCAL_IPS：缺别名会直接退出并原样打印缺的那几条补挂命令。


## 快速开始

```bash
# 构建压测客户端（release 用于 profiling 更准）
cmake --build build --target imap_client smtp_client fsm_bench dispatch_bench -j 4
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target imapsServer imap_client -j 4

# IMAP 读路径吞吐基准（SELECT+FETCH）
python3 test/bench/imap/seed_imap_data.py --mails 200          # 首次灌数据
./build/imap_client --t 16 --conns 4 --rounds 500              # 高并发吞吐
./build/imap_client --t 4 --conns 1 --rounds 500 --select-only # 纯 SELECT 对照

# IMAP 高负载采样找热点（append 进 imap/REPORT.md）
# 需先启动一个 imapsServer（配置见 profile.sh 的 --config / env CONFIG）
./test/bench/imap/profile.sh --sample-secs 10 --topn 20

# SMTP 峰值（见 smtp/REPORT.md 的"运行方法"）
./test/bench/smtp/bench.sh
```

## 约定

- **吞吐基准**：`*_client.cpp` 输出 rounds/s + P50/P95/P99，结果记录在各协议 `REPORT.md`。
- **热点分析**：`profile.sh` 采样服务 PID → 折叠 top-N 热点表 → append 进 `imap/REPORT.md`。
  **必须 profile release 构建**（Debug 热点误导），采样窗口要短于压测时长（否则后半段全是空闲帧）。
- **数据准备**：IMAP 读路径不产生新数据（`seed_imap_data.py` 只灌一次，重复跑是重读）；
  压测邮箱大小影响结果（`get_mailbox_mails` 返回全量），对照时保持同一邮箱规模。
- **平台**：macOS 用内置 `sample`（零依赖）；Linux 用 `perf`（`perf record -p` 通常要 sudo）。
