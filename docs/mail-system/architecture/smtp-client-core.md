# 共享 SMTP 客户端与提交服务边界

## 2026-10-18 议定的分层

Outbound 与 Webmail Submitter 共用 SMTP 客户端内核。内核只处理一个端点上的
连接、TLS、SMTP 响应、EHLO/可选 AUTH、MAIL/多 RCPT/DATA、超时与取消；
返回结构化结果，不认识 HTTP、MX、数据库、租约、DKIM 或发信配额。

| 层 | 责任 |
| --- | --- |
| SMTP client session | 单连接顺序事务、响应限额、Accepted/Rejected/Unavailable/Unknown 等结果 |
| SMTP connection pool | 连接复用、空闲回收、有界并发；按 endpoint/TLS/认证身份分组 |
| Webmail submitter | 固定提交端点、凭据与提交策略；API 层映射 HTTP |
| Outbound 调度 | MX/候选 failover、outbox 租约、内容加载/签名、重试及状态回写 |
| MailSubmissionService（后续） | 各协议共用的用户身份、账号政策、配额、别名、正文存储和持久化入口 |

每条连接创建时从 ExecutorProvider 获取一次 executor。装配根的 provider 捕获
IOThreadPool 的 shared_ptr，并调用既有 get_io_context() 轮询；同一连接的
socket/timer/状态只在固定 strand 上操作，不在每次读写时重新分配。池的 start/stop
归装配根，保活并不等于允许组件自行停止池。

## 重试与接受边界

内核执行一次连接/事务尝试，不自行重新发信。结果保留阶段、SMTP 状态码和每个
RCPT 的结果。连接失败或明确未接受的临时失败可由外层有限重试；永久拒绝/凭据错
不自动重试。DATA 可能已送出而最终应答丢失必须返回 Unknown；收到最终 250 后，
QUIT 失败不改变 Accepted。Submitter 的短期限重试与 Outbound 的持久化退避共享
结果分类，但预算和 Unknown 处理策略分别决定。

当前配额在 MAIL FROM 占用；引入提交事务重试前，须先明确一次逻辑请求的配额
计费与幂等契约。outbox 租约用于任务恢复/竞争，不保证外部投递 exactly-once。

多收件人默认任一 RCPT 拒绝即不发送 DATA；需要部分投递时由调用者显式选择。
连接仅在事务成功或明确复位后复用；协议错误/取消/不确定状态关闭连接。

## 后续可信提交入口（尚未实现）

同进程部署优先注入 MailSubmissionService 接口，SMTP/Webmail 都是其前端，
不让 Webmail 持有具体 SmtpsServer 指针，也不复制一套数据库/文件写入逻辑。
持久化队列和 outbox 共用。默认在正文完成存储及 DB/outbox 事务提交后应答，
不等待外域投递。当前 PersistentQueue.accepted 只表示进入内存队列，不能当作
崩溃可恢复的接受凭据；完成通知应改为回调，替代 worker 的 sleep 轮询。

跨进程可考虑专用 TLS listener/Unix socket。连接认证服务身份（可查数据库中的
服务账号/令牌），每条请求仍携带用户身份、信封、正文长度、request_id，进入同一
提交服务检查账号/发件人/配额/别名，再持久化。需定义消息边界、在途窗口、背压、
请求结果查询，以及持久化幂等键（同键不同内容拒绝）。现有 Message-ID 去重不
直接充当请求幂等凭据。TLS、长连接、认证一次本身也可由 SMTP 提供；自定义协议
的增益在可信身份委托与幂等提交，而不是单纯省握手。

不会只把 Webmail 改指向公开 25 并加 trusted_relay_networks：当前该白名单仅在
RCPT 放行中继，不能维持 AUTH 路径上的配额、发件人绑定、外投开关与别名语义，
也不是 DNSBL/SPF/DKIM/DMARC 的统一豁免开关。

## 本轮实施范围

先提取公共客户端，迁移 Submitter/Outbound 并接上 executor provider；保留既有
Outbound 池与 MX/outbox 编排。通用连接池、Submitter 自动重试、共享提交服务、
配额幂等与自定义 TLS 协议后续分步落地。本轮不更改公开 25/587 的受信策略。
