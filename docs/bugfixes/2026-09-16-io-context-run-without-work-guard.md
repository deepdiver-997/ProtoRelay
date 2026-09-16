# io_context::run() 不装 work_guard —— 发起队列静默饿死（CI 连红 18 天的最后一块拼图）

- 日期：2026-09-16 定位并修复
- 影响：CI 的 unit-test / coverage / asan-unit 三个 job 自 2026-08-28 起连红；
  smtps/imaps/pop3 三个 FSM 测试 + 并发测试挂在第一个协议断言（220 greeting /
  CAPABILITY / +OK / SPF pending）。
- 生产影响：无（真实服务器由 io 线程常驻 run()，不受影响）。纯测试基建问题。

## 现象

CI 四个测试二进制全部在**会话的第一个协议响应**上断言失败：

```
smtps_fsm_test.cpp:196:  HAS(w, "220 SMTPS Server") failed
imaps_fsm_test.cpp:122:  HAS(w, "CAPABILITY IMAP4rev1") failed
pop3_fsm_test.cpp:208:   HAS(w, "+OK") failed
smtps_fsm_concurrency_test.cpp:215: has_pending_txt("attacker.com") failed
```

本地 macOS 同样复现。服务端日志无异常；FSM 状态停在 GREETING（handler 已执行、
`do_async_write` 已调用），但 MockConnection 的 `written()` / `captured` 恒为空。

## 根因（两层）

1. **09-06 发起串行化（95bd2cd）**：`SessionBase::do_async_write` / `close` 的真实
   动作改为 `boost::asio::post` 到连接 executor——这是正确的并发修复，但**写回了
   executor 队列**，完成时序从同步变异步。
2. **测试 mock 的 executor 没人跑**：`mock_connection.h` 的 `get_executor()` 返回真实
   `exec_ctx_`（boost io_context），fixture 未以任何方式运行它 → post 进去的写任务
   永远排队。而 `session_base_test.cpp` 在 95bd2cd 同期已适配（全部改用
   `pump_executor()`），四个 FSM 测试文件漏掉了。

修复过程中又叠加发现 **run() 不装 work_guard 的饿死陷阱**：给 `exec_ctx_` 起线程跑
`run()`，线程启动瞬间因无任务立即返回；之后所有 post 只入队、永不执行——与
"没起线程"现象完全相同，无任何报错。必须 `make_work_guard` 持活（guard 放线程栈，
stop 时 reset → stop → join → restart）。

## 修复（2026-09-16，本地 24/24 全绿）

1. **四个 FSM 测试文件适配发起串行化**：
   - 驱动点后补 `pump_executor()`（greeting/capability/CMD/start helper）；
   - 等待循环（wait_for_response/wait_for_reply/wait_for/wait_closed 及各内联轮询）
     轮询体内加泵——异步回复（bcrypt 认证、SPF 回调）落在 exec_ctx_ 队列，只睡不泵
     永远等不到；
   - 落盘扫描/DATA 完成/FETCH 完成等异步段落改为轮询等待完成标记。
2. **并发 fixture**：`run_on_io` 增加 `start_executor()`；mock 的 `exec_ctx_` 改为堆上
   `shared_ptr`（见下）、后台线程 + 栈上 work_guard。
3. **mock 析构自 join 防护**：`close_after_flush` 的 post 持有 session 最后引用时，
   session/conn 会在 exec 线程内析构 → `stop_executor` join 自己 →
   `Resource deadlock avoided`。检测到自 join 时 detach（ctx 由线程 lambda 的
   shared_ptr 续命，run() 排空后自然释放）。
4. **产品侧同类修复**（同族时序问题的三处真实路径）：
   - `SessionBase::close_after_flush()`：worker 回调里 "末条响应 + close" 组合不再
     丢消息（close post 排在写 post 之后 FIFO）；SMTP/IMAP/POP3 三处认证阈值关闭、
     POP3 QUIT 两处、IMAP literal 超限关闭全部改用；
   - `PersistentQueue`：无 DB 池的部署（use_database=false）改为文件模式持久化成功，
     不再按入库失败清理已接受的邮件正文。

## 经验

- **改共享层的完成时序（同步→post 异步）时，全仓库的测试驱动模式都要过一遍**——
  `session_base_test` 修了，四个 FSM 测试文件漏了，CI 红了 18 天没人看。
- "post 进去了但回调永不执行、无报错" = executor 没人跑 / run() 提前退出。
  第一反应查 work_guard，而不是怀疑业务逻辑。
- 静默失效类问题（本例 + 5085ddb）的共同点：没有崩溃、没有日志，只有功能不工作。
  测试断言必须覆盖"响应真的到达了对端"。
