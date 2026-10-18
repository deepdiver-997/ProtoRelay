# 共享 SMTP 客户端与提交服务边界

## 当前分层（2026-10-18）

Outbound 和可注入的兼容 LoopbackSmtpSubmitter 共用 SMTP 客户端内核。
默认 Webmail 已使用 DirectSubmitter → MailSubmissionService → PersistentQueue，
不再进行 loopback SMTP AUTH。内核只处理单端点连接、TLS、响应、EHLO/可选 AUTH、
MAIL/多 RCPT/DATA、超时与取消，不认识 HTTP、MX、DB、租约、DKIM 或配额。

| 层 | 责任 |
| --- | --- |
| SMTP client session | 单连接顺序事务、有界响应与结构化结果 |
| LoopbackSmtpSubmitter | 显式注入的 SMTP 兼容/测试适配器，每请求独立连接 |
| DirectSubmitter / MailSubmissionService | 默认 Webmail；身份、政策、别名、正文和持久化结果 |
| AccountPolicy | SMTP/Webmail 共用的认证、发送者绑定、配额及别名 |
| OutboundServer / OutboundSmtpSession | MX/候选 failover、连接复用、outbox 租约、签名、重试 |

通用 SMTP 连接池尚未抽出；Outbound 保留自己的 MX 池。没有独立 outbound 可执行文件，
模块由 SMTP/Webmail 应用装配；独立中继节点运行 SMTP 进程。

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

## 进程内提交已实现；跨进程可信入口待做

默认 Webmail 已注入 MailSubmissionService；SMTP FSM 继续经共享 AccountPolicy 和队列工作，
不让 Webmail 持有具体 SmtpsServer 指针，也不复制一套数据库/文件写入逻辑。
持久化队列和 outbox 共用。默认在正文完成存储及 DB/outbox 事务提交后应答，
不等待外域投递。PersistentQueue.accepted 只表示进入内存队列，不能当作
崩溃可恢复的接受凭据。第二阶段已增加完成回调并移除 SMTP worker sleep 轮询，
详见 [持久化完成通知](persistence-completion.md)。

跨进程可考虑专用 TLS listener/Unix socket。连接认证服务身份（可查数据库中的
服务账号/令牌），每条请求仍携带用户身份、信封、正文长度、request_id，进入同一
提交服务检查账号/发件人/配额/别名，再持久化。需定义消息边界、在途窗口、背压、
请求结果查询，以及持久化幂等键（同键不同内容拒绝）。现有 Message-ID 去重不
直接充当请求幂等凭据。TLS、长连接、认证一次本身也可由 SMTP 提供；自定义协议
的增益在可信身份委托与幂等提交，而不是单纯省握手。

不会只把 Webmail 改指向公开 25 并加 trusted_relay_networks：当前该白名单仅在
RCPT 放行中继，不能维持 AUTH 路径上的配额、发件人绑定、外投开关与别名语义，
也不是 DNSBL/SPF/DKIM/DMARC 的统一豁免开关。

## 阶段记录

以下第一、第二阶段描述各自实施时的范围。当前默认直接提交、共享账号政策和
共享单会话已落地；通用连接池、自动提交重试、请求幂等与跨进程可信协议仍待做。
公开 25/587 的受信策略未因 Webmail 改变。
## 第一阶段实现（2026-10-18）

- `smtp_client/client_session.{h,cpp}`：可复用的单连接异步客户端，支持明文、
  implicit TLS、强制 STARTTLS（TLS 至少 1.2，验证 CA/主机名，升级后重新 EHLO），
  可选 AUTH PLAIN、多 RCPT、DATA、QUIT、有界分片响应解析和每操作 deadline。
  新连接由调用方提供 executor，内核建立固定 strand。取消操作持活旧连接及其
  缓冲，直到挂起的 I/O 回调退出，允许安全重连；同连接同时发起第二个操作会返回 busy。
- `smtp_client/secret.h`：进程内共享凭据及销毁擦除；Webmail 的 SessionPassword
  保留原名称作为别名，内核不依赖 Webmail。
- `LoopbackSmtpSubmitter` 已成为薄适配器：固定 loopback/AUTH、每次提交从
  ExecutorProvider 取一次 executor，连接和事务共用原来的总时间预算；最终 250
  后立即完成 HTTP 所需回调，QUIT 异步收尾不影响结果。仍然每请求独立连接。
- `OutboundSmtpSession` 已成为出站策略适配器：保留 MX 候选、三轮连接重试、
  最多 100 封后换连接、空闲 30 秒退出。加载/签名在 worker，队列和定时器在
  控制 strand；每个新连接单独选 executor，连接完成后再回控制 strand 推进队列。
  删除 `OutboundSmtpFsm` 及其事件/状态 enum，不保留第二套协议实现。
- Outbound 完成结果改为结构化结果：Accepted→sent，5xx→dead，其余暂缓结果
  （含 Unknown）→retry，延迟 30 秒并遵守现有 max_attempts。详见
  [专项回归](../../bugfixes/2026-10-18-outbound-result-completion.md)。连接耗时指标
  现在计量 resolve/连接/SMTP 就绪的完整尝试，不能直接与旧版仅 TCP 阶段数值比较。

本阶段只实现全有或全无 RCPT 策略，失败即关闭；未提供部分投递和 RSET 复位 API。
当时两个适配器使用既有明文 SMTP 端点（兼容提交器仅 loopback），TLS 能力经公共
内核 API 提供，未新增部署配置或自动更改现有端点。报文仍整体缓存在内存中；
当前仍待通用连接池、流式 SMTP 正文和自定义可信协议；共享进程内提交服务已实现。

## 第一阶段验证记录

```sh
cmake --build build-release -j 8
ctest --test-dir build-release -j 4 --output-on-failure
.venv/bin/python test/scripts/webmail_send_smoke.py --db-config config/db_config.json
cmake --build build-asan --target smtp_client_test smtp_submitter_test outbound_smoke -j 6
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-asan -R 'smtp_client_test|smtp_submitter_test|outbound_smoke' --output-on-failure
```

结果：Release 全构建通过，32/32 CTest 通过；三个专项在 ASan/UBSan 下通过
（macOS 不支持该 leak detector，禁用 detect_leaks）。Outbound 复用/failover
另以控制队列和连接分属不同 IO context/线程运行并通过 ASan/UBSan。Python
独立 MIME 消费者及真实 HTTP 登录→SMTP AUTH→持久化→To/Cc 收件箱/正文读取、
配额与注销检查均通过。测试只向本机投递，未作外部账号收信验收。

## 第二阶段完成

已落实持久化完成订阅与 SMTP after_persist 接入，见
[持久化完成通知](persistence-completion.md)。后续阶段已实现 AccountPolicy 和
默认直接提交；可信 listener、请求级持久化幂等仍未实现。

## 2026-10-18 续：共享账号政策与会话已落地

AccountPolicy 已从 SMTP/Webmail 认证、别名、配额路径收口；Webmail 默认使用
注入的 MailSubmissionService，确认 PersistentQueue 终态后响应。共享 DB 会话
支持跨节点和重启；新登录替换旧设备会话，生产不保留 SMTP 密码。详见
[共享会话与提交服务](webmail-sessions-and-submission.md)。可信 TLS listener、
自定义协议、幂等键/配额补偿、会话管理 API 仍未实现。
