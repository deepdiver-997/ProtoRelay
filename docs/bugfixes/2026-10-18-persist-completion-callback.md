# 2026-10-18：after_persist 饥饿与持久化完成边界

## 三条根因

1. SMTP after_persist 把 sleep 轮询任务提交到与持久化共用的 worker 池。当等待先
   入池、持久化后入池，单 worker 会在等待中占住唯一线程，持久化无法开始，直到
   本不必要的超时。多 worker 在并发请求下同样可能被耗尽。
2. 旧成功回调在 COMMIT 后检查 cancel_requested，并调用 cleanup_failed_mail。
   超时/取消可能因此删除已经提交的邮件和正文。
3. 旧失败分支发起异步 rollback 后立即回调失败，ScopedConnection 可能在 rollback
   完成前归还；调用方也无法把失败通知当成事务已经结束的依据。

## 修复

引入 PersistSubmissionTicket 的完成订阅与异步 deadline，完成通知只发布一次，支持
完成前/后订阅。SMTP 在连接 executor 上等待，不占 worker；超时结束等待但不伪造
持久化终态。取消仅在开始前执行，成功提交的邮件不因晚到取消被删。

失败事务等待 rollback 回调后发布 FAILED，并在整条回调链中同时持有 DBPool 和连接
租约。队列关机拒收、排空与背压也纳入相同通知路径；原先的 idle sleep 改为条件变量。
已接受任务的在途计数在终态路径释放，析构等待在途回调完成。

## 验证者视角与双向验证

- `smtp_persist_ack_test` 用单 worker，并可控地延迟 dedicated queue thread 的首次
  投递，确定性制造“等待先入池、持久化后入池”。等待期间投递一个普通任务，要求
  它能执行；持久化释放之前不能看见 250，释放后必须收到 250 且正文保留。
  对原实现先运行得到 exit 1：`after_persist occupied the only worker while
  persistence was waiting`；新实现 PASS。
- `persist_submission_test` 的手动 COMMIT 对端在已开始事务时请求取消，然后确认
  COMMIT。验证消费者得到 SUCCESS、数据库未执行删除、正文仍存在。临时恢复旧
  成功回调中的取消清理分支（使用新通知接口表达同一 CANCELLED 结果）后得到
  SIGABRT / exit -6，失败在“结果必须 SUCCESS”的断言；恢复修复后 PASS。
- 同一测试用手动 ROLLBACK 对端制造元数据失败，在 rollback 完成前要求消费者尚未
  得到失败通知。恢复旧“发起 rollback 后立即回调失败”的顺序后得到 SIGABRT /
  exit -6，失败在等待仍应 pending 的断言；恢复修复后 PASS。

两个恢复旧语义的编译副本仅在 `/tmp/persist-regression`，没有修改工作区文件。
最终测试另外链接原版轮询 FSM 复跑，仍 exit 1（`/tmp/persist-negative-final-test.log`）。
负验证日志还包括 `/tmp/persist-negative-test.log`、`/tmp/persist-negative-cancel.log`、
`/tmp/persist-negative-rollback.log`。

还覆盖完成前/后/并发订阅、重复完成、COMMIT 失败后回滚、取消未开始任务、关机、
背压返还所有权、等待超时后晚到成功只应答一次。SMTP 并发夹具按其 use_database=false
配置使用独立的文件模式队列，避免原有单槽手动 MockDbConnection 的回调被后台
持久化覆盖；DB 事务边界由专门的手动 COMMIT/ROLLBACK 夹具验证；真实 MySQL 路径另外通过
HTTP→SMTP→持久化→读信 smoke。

运行命令及接受边界见
[架构说明](../mail-system/architecture/persistence-completion.md)。
