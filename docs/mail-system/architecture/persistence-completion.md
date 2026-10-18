# 持久化完成通知：共享提交服务的接受边界

## 第二阶段（2026-10-18）

在第一阶段共享 SMTP 客户端之后，先落实 MailSubmissionService 所需的提交结果
通路。SMTP 的 after_persist 已迁移到此通路，Webmail 通过现有 SMTP 提交也随之
使用。认证、配额和别名仍由现有 SMTP 流程执行；本阶段不提供 Webmail 直接入库
或新的可信 listener。

## 接口及保证

`submit_owned_mail(unique_ptr<mail>)` 保留入队结果及所有权契约：

| 返回/通知 | 含义 |
| --- | --- |
| accepted=false | 未接收，rejected_mail 返还所有权，error 给出背压/关机等原因 |
| accepted=true | 已进入内存队列，得到 PersistSubmissionTicket；不表示崩溃可恢复 |
| ticket.async_wait(executor, timeout, callback) | 非阻塞等待，回调总在指定 executor 的 strand 上执行 |
| SUCCESS | DB 模式的主分片元数据/outbox 事务已经确认提交；去重命中另带 deduplicated=true |
| FAILED / CANCELLED | 处理失败或在开始之前取消；失败事务等待 rollback 完成才发布结果 |
| timed_out=true | 该次等待结束，持久化终态可能尚未确定；不改写共享持久化状态 |

SMTP 已先经存储 provider 完成正文 commit，再移交给队列。队列接口接收的是
**正文已经提交的 mail**，不负责替调用方刷盘。无 DB 的既有文件模式保留原语义：
正文文件是事实来源，不产生 outbox。

完成状态只发布一次，支持完成之前、之后以及同时订阅；重复完成不能覆盖结果。
多个等待者可分别设置 deadline，超时只结束相应等待。过期等待不再保留 session
回调，持久化晚到完成也不触发第二次 HTTP/SMTP 应答。

## 取消、超时和关机

取消请求在任务开始之前检查。事务开始之后不撤回已提交邮件；尤其不能在 COMMIT
成功回调中因为 cancel_requested 而删除数据库和正文。async_wait 超时仅结束等待，取消由调用方显式请求。SMTP 超时会请求取消未开始
任务，但已经开始的事务仍可能提交成功；随后保留现有 451 并关闭连接，不能
将 451/断连理解为“数据库中一定没有邮件”。请求级持久化幂等与结果查询仍待实现。

队列 admission 与 shutdown 使用同一互斥门闩：先预占在途数，再将任务交给消费者；
队列满直接返还所有权。空闲消费者通过条件变量等待。shutdown 拒绝新任务，取消
尚未开始的任务，并让已开始的事务完成；析构等待在途回调收敛，避免回调访问已释放
的队列。装配根必须让 worker 和 DB 运行时在排空期间保持可用，不能先停其执行器。
事务回调同时持有 DBPool 与 ScopedConnection，rollback 完成前不归还租约。

## SMTP 接入

after_persist 不再向 worker 池提交 sleep 轮询任务。session 暂停命令处理，在连接
executor 上用 ticket 的完成通知/定时器竞争一次应答：成功后 250 并恢复读，失败或
超时后 451 关闭。已关闭的 session 忽略晚到通知。after_enqueue 仍由配置显式选择，
维持原来的内存入队后应答语义，不把它改称 durable acceptance。

这一步让后续同进程/可信 listener 的提交服务能等待相同的事务完成通知，不需要
持有 SmtpsServer 指针，也不需要再造一套持久化轮询。下一步再抽取认证身份、发件人
约束、配额和别名的共享业务策略，并提供 MailSubmissionService 的完整提交入口。

## 验证

专项测试为 `persist_submission_test` 和 `smtp_persist_ack_test`，覆盖受控的单 worker
饥饿时序、提交前无 250、提交与取消竞争、超时后晚到成功、回滚完成边界、完成前后
订阅、并发通知、关机及背压。负验证和根因见
[专项复盘](../../bugfixes/2026-10-18-persist-completion-callback.md)。

```sh
cmake --build build-release --target persist_submission_test smtp_persist_ack_test -j 8
ctest --test-dir build-release -R 'persist_submission_test|smtp_persist_ack_test' --output-on-failure
.venv/bin/python test/scripts/webmail_send_smoke.py --db-config config/db_config.json
```

本轮验收：Release 全构建及 34/34 CTest 通过；`persist_submission_test`、
`smtp_persist_ack_test`、`smtps_fsm_concurrency_test` 在 ASan/UBSan 下通过
（macOS 使用 ASAN_OPTIONS=detect_leaks=0）。真实本机 HTTP 登录→SMTP AUTH→
持久化→To/Cc 收件箱与正文读取、配额及注销闭环通过，未向外部账号发信。
SMTP 专项还验证关闭的 session 不再应答、超时只回一次 451、正文包含实际输入内容；
两个观察者的独立 deadline 不会互相取消后台任务。
