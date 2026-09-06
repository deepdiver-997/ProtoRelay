# 2026-09-06 — ASan soak 战果：三个内存/生命周期 bug 的定位与修复

09-05 起 IMAP 服务器（imapserver）连环崩溃（`malloc(): unaligned tcache chunk
detected` → ABRT，systemd 反复拉起），另有"进程活着但拒绝服务"形态。
IMAP 换 ASan+UBSan 版浸泡后，两个独立 UAF 先后现形；外加一个早已 gdb 实锤的
阻塞 bug。三个全部修复，ASan 版部署生产继续浸泡。

## Bug 1：stmt 截断重取路径 heap-use-after-free（ mysql_service.cpp）

`MySQLConnection::query` 截断重取路径（09-05 d3a739c 引入）：
`mysql_stmt_bind_result` 把 bind 数组**按值拷贝**进 stmt 内部；列值 >256 字节
触发 `MYSQL_DATA_TRUNCATED` 后，`buffers[i].resize()` 释放旧 256 字节缓冲、
只更新**本地** bind 指针 → stmt 内部仍指已释放块 → 下一行 `mysql_stmt_fetch`
memcpy 写悬垂指针。ASan：`WRITE of size 256 ... freed by ...`，分配/释放/访问
三栈收敛于 mysql_service.cpp:408/434/426。

触发面 = 任何查询任何列 >256 字节；收件箱列表 subject 列（257~998 字节主题，
中文 ×3B）必现 → IMAP 天天崩，崩溃时刻对齐网易邮箱大师 :27s 轮询，与肇事写
解耦（tcache 元数据坏了要等下一次 malloc 才爆）。

**修复**：resize 后 `mysql_stmt_bind_result(stmt, result_binds.data())` 整体
重绑同步 stmt 内部副本，再 `fetch_column`。`mariadb_service.cpp` 两处同款
（备选后端，预防性）。验证：修复版上线后同一轮询时刻干净通过，ABRT 归零。

## Bug 2：会话生命周期 UAF —— 看门狗 handler 掉最后一个引用（09-06 soak 抓到）

asan.714738（09:05，修复版二进制上）：
- **释放**：读超时看门狗 `rearm_impl` 的 `async_wait` handler 完成退出时析构其
  `shared_ptr<SessionBase>` —— 恰为最后一个引用 → `~ImapsSession` →
  `~SslConnection`（ssl_connection.h:29）→ ssl::stream（440B）随会话销毁；
- **访问**：同 socket 上**队列里还有一笔无主的 flush write**（close() 刷
  pending_write_buf_ 的 op，handler 未捕获 self）——ssl 多阶段 op
  （`ssl::detail::io_op`）持 stream core **裸指针**继续运行错误阶段，
  `expires_at()` 读已释放 stream 内部 timer（io_object_impl.hpp:121）。

**修复**（生命周期不变量：**每个组合异步操作的最终完成回调必须持有
shared_ptr；所有 socket 发起与关闭在连接 executor 上串行；close 不做同步
shutdown**）：
- `session_base.tpp close()`：flush op 捕获 `self`，保证会话活到 op 全部
  走完才析构；析构期安全网路径（shared_from_this 不可用）直接跳过 flush
  ——该路径无在途 op（op 回调都持 self，析构不可能发生）。
- **发起序列化**：`do_async_read/do_async_write` 的 socket 发起一律
  `post` 到连接 executor 并在 post 内复查 `closed_`；`close()` 的
  socket 关闭同样 post 到该队列（flush 先、关闭后，FIFO）。asio 允许
  "close 有 pending op 的 socket"（取消语义），但**禁止 initiate 与
  close 并发**——worker 线程（DB 回调续跑）发起 op 与 IO 线程 close 的
  竞争由此消除。`closed_` 用 `exchange(true)` 原子去重，并发 close 只跑一份。
- 定时器保持在会话层（会话策略语义），不下沉传输层——bug 与定时器
  归属无关，下沉只会加重耦合。

## Bug 3：SslConnection::close() 同步 SSL_shutdown 阻塞 IO 线程（gdb 早前实锤）

`ssl_connection.h close()` 的 `stream_->shutdown(ec)` 是同步 SSL_shutdown：
阻塞等对端 close_notify，对端沉默（手机停靠连接/硬断开）= `poll(-1)` 永久
卡死 IO 线程 → 服务器照常 accept+握手但永不回包（09-05 17:29 全网断连，
gdb 实锤两 IO 线程卡死于此）。watchdog 的 cancel 救不了同步阻塞。

**修复**：去掉同步 shutdown，直接 `lowest_layer().close(ec)` 关 TCP 层。
副作用：服务器不再发 TLS close_notify，极少数做优雅关闭的客户端可能记录
一条 SSL 警告（无害，imaplib/大师等主流客户端本就直接关 fd）。

## 设计答疑（本次定下的不变量）

- "cancel 在途 ssl 任务，completion 会以 aborted 提前触发，不就不碰已析构的
  stream 了吗？"——只对一半：cancel 让**最内层 TCP op** 以 aborted 完成，
  但 ssl 多阶段包装层仍会进入错误处理阶段摸 stream。真正的保命机制是
  **顺序**：完成回调持 self → 会话必然活到 op 全链走完 → 析构必在所有 op
  之后。cancel 负责"快走完"，self 负责"走完前别死"，缺一不可。

## 验证

- 本地 ASan 版：16 会话（硬断开/优雅 LOGOUT 交替）全绿，零报告；
- 生产：gold 链接 ASan 版部署，LOGIN 冒烟通过；Bug1 修复经 01:25:27 真实
  轮询验证（修复前 5 连崩，修复后归零）；Bug2/3 由浸泡持续观察
  （Bug2 修复前 8h 出现 1 次，观察窗口需 ≥24h）。

## 诊断基建（复用指南）

`docs/local/asan-campaign-runbook.md`：ASan 交叉构建/部署全流程、gold 链接
（bfd 在 2G 内存机器上必 OOM）、`--exclude webServer_obj` 必要性、watchdog
与巡检的退役步骤。本地复现管线：`build-asan-diag/imapsServer` +
`/tmp/asan_client_driver.py` + `/tmp/asan_nasty_hammer.py`。

## 关联

- d3a739c（09-05）：修 DATA_TRUNCATED 中止结果集，引入 Bug 1；
- 2299575（09-05）：同段代码先行排查（fetch 错误日志补 rc）；
- 8146aa0（09-05）：09-04/09-05 两起事故复盘（subject 超长静默丢失等）。
