# 2026-10-18：Outbound 的拒绝与连接失败必须完成任务

## 现象与根因

抽取 SMTP 客户端内核时发现：旧 `OutboundSmtpFsm` 仅在 WAIT_ACCEPT 注册了
4xx/5xx 处理器，MAIL FROM、RCPT TO、DATA 和 DATA_BODY 的错误没有完成回调。
连接三轮重试耗尽也只将 session 标记 CLOSED。调度器因此无法及时递减 pending
水位或回写状态，只能等待 outbox 租约恢复。旧完成接口的 bool 也无法表达临时
失败和 DATA 最终确认丢失。

## 修复边界

删除旧出站协议 FSM，由共享 `smtp::ClientSession` 返回阶段、完整 SMTP 数字码、
逐 RCPT 结果和结构化 outcome。Outbound 策略适配器对队列中的每个运行任务只
完成一次；连接候选与有限重试耗尽返回临时失败。调度器映射如下：

| 结果 | outbox 操作 |
| --- | --- |
| DATA 最终 250 | mark_sent |
| 明确 5xx / 凭据错误 | mark_dead |
| 4xx、连接/超时/协议失败、OutcomeUnknown | mark_retry（30 秒，受行中 max_attempts 限制） |

Unknown 不在当前连接内立即重发。Outbox 后续按 at-least-once 策略重试，因此在
对端已接受但 250 丢失时仍可能重复投递。Webmail 不自动重试 Unknown。
关机取消未完成任务不回写终态，由既有租约恢复。

迁移后的 session 不再继承服务端 SessionBase；队列与重试/空闲定时器固定在
控制 strand，连接 I/O 固定在各自 strand，正文加载/签名在 worker 池执行。
调度器定时器也通过固定 executor 发起/取消，异步回调以 weak_ptr 检查生命周期，
资源由装配时复制的共享依赖持有，不再捕获具体 SMTP server 裸指针。

## 回归与双向验证

`test/unit/outbound_smoke.cpp` 替换原先只检查 enum 的 smoke：真实 loopback 对端
读取信封和 DATA，覆盖同连接连续两封、首 MX 拒连后 failover、MAIL 550、RCPT
550/450、DATA 451、正文最终 552、正文后断连以及三轮连接拒绝后的逐任务回调。复用/failover 场景的控制队列与连接运行在不同 IO context/线程。
收到 RCPT 拒绝后，对端脚本没有 DATA 步骤。

负验证使用 `bde168d` 的旧 session/FSM/types 构建独立 probe：真实 peer 回应
220 → EHLO 250 → MAIL 250 → RCPT 550，两秒内要求完成回调。结果 exit 1：
`FAIL: legacy RCPT 550 lost completion callback`。恢复新实现后，对应的
`outbound_smoke` 场景 PASS。临时 probe 与日志位于 `/tmp/smtp-legacy-probe`、
`/tmp/smtp-core-legacy-test.log`，不进入源树。

运行命令：

```sh
cmake --build build-release --target outbound_smoke smtp_client_test smtp_submitter_test -j 8
ctest --test-dir build-release -R 'outbound_smoke|smtp_client_test|smtp_submitter_test' --output-on-failure
```

公共内核另有分片响应、TLS/STARTTLS、证书链/主机名拒绝、AUTH、取消后重连和
最终确认丢失测试；Submitter 测试验证并发请求实际分布到两个 IO context。

补充时序回归：退避重试期间再 submit 第二个任务，不得取消第一个任务的 retry
定时器。对初版适配器的双向验证先得到 SIGABRT（exit -6）：
`queued submit cancelled connection retry`；修改为只在空闲时取消 idle timer 后，
两个任务都能各完成一次。已排队的旧 idle 回调也检查当前任务/连接状态，防止在
新事务期间发 QUIT。
