# Webmail HTTP API

`webmailServer` 是复用邮件引擎的第四个协议前端。当前支持 cookie 登录会话、
邮箱列表、邮件分页、MIME 正文读取与清洗、通过共享 MailSubmissionService 写信。
React/TypeScript SPA、已读/标星、附件下载及自动保存已发送副本已实现。
附件上传、服务器草稿、全库搜索、文件夹修改尚未完成。前端构建与开发代理见
[Webmail 前端](../../../webmail/README.md)。

## 启动与装配

```bash
./build-release/webmailServer --help
./build-release/webmailServer config/webmailConfig.json 8081 ./build-webmail
```

参数顺序为配置文件、HTTP 端口、静态根目录。`webmail_node_id` 为 0..1023
的写节点编号，默认 1；所有写同一邮件库的节点须唯一，避开 SMTP 默认 0。
生产配置 `use_database=true`，在 shard 0 应用 `migration_web_sessions.sql`；
上一版多会话表还须停旧节点后运行 `migration_web_sessions_single_login.sql`。
原 `webmail_submission_port` 不再读取。Webmail 自己装配配额、外域开关、存储和
outbound 配置，须与 SMTP 一致。详见
[共享会话与提交服务](webmail-sessions-and-submission.md)。
TLS/Secure cookie 感知装配仍待补；静态目录不能指向邮件正文存储。

## 端点

| 方法与路径 | 行为 |
| --- | --- |
| `GET /api/health` | 存活探针 |
| `POST /api/login` | `{address, password}` → 新 cookie，撤销该账号旧会话 |
| `POST /api/logout` | 删除会话并清 cookie |
| `GET /api/me` | 当前用户 id/address |
| `GET /api/mailboxes` | 本人邮箱及 total/unread |
| `GET /api/mailboxes/{id}/messages?page=1&page_size=50` | 邮件分页；每页上限 100 |
| `GET /api/mailboxes/{id}/messages/{mail_id}` | 邮件元数据、解码正文、附件清单 |
| `PATCH /api/mailboxes/{id}/messages/{mail_id}` | 本人的 read/is_starred 状态 |
| `GET /api/mailboxes/{id}/messages/{mail_id}/attachments/{attachment_id}` | 鉴权后流式下载附件 |
| `HEAD /api/mailboxes/{id}/messages/{mail_id}/attachments/{attachment_id}` | 同样鉴权，返回长度/下载头，不发送正文 |
| `POST /api/send` | 生成 MIME，按会话身份提交共享 MailSubmissionService |

用户、邮箱、邮件、附件的 `id` 一律为十进制 **JSON 字符串**（如 `"9007199254740993"`），
避免 JavaScript Number 丢失雪花 ID 的精度；计数、时间戳、状态仍为数字。JSON 响应
设置 `Cache-Control: no-store` 和 `X-Content-Type-Options: nosniff`。

邮件列表与详情的 `subject` 解码 UTF-8/ASCII RFC 2047 B/Q encoded-word；
未知 charset 保留原文。HTML 正文经过白名单清洗；前端仍须使用无脚本、
无 same-origin 权限的 sandbox iframe 和 CSP，远程图片默认禁用。

## 已读与标星

`PATCH` 要求登录 cookie 和 `Content-Type: application/json`，请求为非空对象，
只允许布尔字段 `read`、`is_starred`，可单独或一起提交。成功返回 `200 {"ok":true}`；
重复设置成功，即使数据库改变行数为 0。未知字段、错误类型/空对象为 400，媒体类型为 415。

先在事务中锁定并验证邮箱/邮件均归本人，再更新。`read=true` 将本人收件行的状态
1 改为 0，false 反向设置；只处理 0/1，保留其他投递状态，其他收件人不受影响。
Sent 的 read 更新不改变任何收件行；Sent 列表/详情状态与未读数均为 0。
标星只更新本人当前邮箱中的关联。详情 GET 本身不改变已读状态，前端打开后显式 PATCH。
没有归属关系统一 404；DB/事务失败回滚并返回 503。

## 附件下载

**入站衔接限制（2026-10-18 实测）**：下载接口依赖有效的独立附件对象和数据库索引。
当前 SMTP DATA 把完整 MIME（含所有附件字节）写入正文文件，但其轻量解析器只在
DATA_END 记录最后一个附件的占位元数据，路径为空、大小为 0，未完整提取独立附件。
因此 SMTP/IMAP 原始 MIME 与 BODY section 收发正常，不代表这些邮件已能经 Webmail
附件接口下载。浏览器 fixture 预置了有效附件文件/索引，验证的是读侧接口；SMTP →
附件提取/索引 → Webmail 的端到端衔接仍需实现并单独验收。

在同一查询中核对附件属于该邮件、邮件在该邮箱、邮箱和关联均属当前用户；
路径从数据库和 shard storage 获取，不接受客户端文件路径。跨用户/不匹配 ID 返回 404。
GET/HEAD 均验证会话。成功使用 octet-stream、attachment Content-Disposition、
UTF-8 filename*（控制字符/引号全部百分号编码）、nosniff/no-store。
HEAD 包括错误响应均不发送正文。

存储提供 `open_download` 有界读取接口；local 持有已打开文件，distributed 在打开时
依次尝试副本，HTTP worker 每次最多读 64 KiB，写完成再读下一块。长度以打开的实际对象
为准；下载中途对象截断或读取失败记录日志并关闭连接，消费者可发现 Content-Length
不完整。每个下载持有 worker 池，不在后续块中依赖裸 Server 指针。
S3/HDFS 尚无有界下载实现时返回 503，不使用 `read_all` 回退；不支持 Range/断点续传。

## 写信请求

请求须携带登录 cookie，`Content-Type: application/json`。限制媒体类型使跨源
简单表单不能触发发信；服务没有放开跨源 CORS。示例：

```bash
curl -b /tmp/webmail.cookies -H 'Content-Type: application/json' \
  --data '{"to":["recipient@example.com"],"cc":[],"subject":"你好","text":"纯文本正文","html":"<p>HTML 正文</p>"}' \
  http://127.0.0.1:8081/api/send
```

`to`、`cc` 为可选地址数组，合计必须为 1..100 个地址，信封收件人按引擎地址唯一键口径忽略大小写去重。
地址为 ASCII addr-spec，不支持显示名、注释、引号 local-part 或 SMTPUTF8 地址。
`subject` 可省略，上限 512 字节，禁止控制字符。`text`、`html` 至少提供一个字符串，
允许空字符串；二者同时提供时生成 multipart/alternative。正文总量上限 512 KiB，
HTTP JSON 请求上限 1 MiB。正文转 CRLF 后使用每行 76 字节的 base64；主题使用
折行 encoded-word，日期为 UTC，Message-ID 和 boundary 使用 CSPRNG。

`from` 与未知字段返回 400；发件人始终取自已认证会话。
`bcc` 当前也返回 400：引擎的 IMAP/读信 API 仍将信封收件人列表用于展示，
在读侧隐私契约完成前不开放密送。附件上传/原始 MIME/自定义邮件头不在此接口中。

生产会话使用 shard 0 的 `web_sessions` 表：每个账号只保留一个 token 摘要，
新登录原子替换旧会话，旧 cookie 后续在所有节点返回 401。多个 Webmail 节点和
服务重启均认可同一当前会话；旧设备的 logout 不会撤销新会话。
不在 users 中保存单个 token，不保留 SMTP 密码。每次请求核对账号最新状态和
密码版本，DB 不可用返回 503。详细迁移与部署约定见
[共享会话与提交服务](webmail-sessions-and-submission.md)。

## 提交结果

默认提交后端确认持久化事务完成后返回：

```json
{"status":"accepted","message_id":"<random@example.com>"}
```

HTTP 状态为 **202**，表示已确认持久化，不表示外域收件人已经收到。
共同策略经 AccountPolicy 执行，正文/元数据经现有存储和 PersistentQueue，
出站继续用 outbox/MX/DKIM。客户端不自行重试；超时不能假设未投递。

| HTTP 状态 | 含义 |
| --- | --- |
| 400 / 413 / 415 | 请求校验失败 / 超限 / 媒体类型错误，未进入提交服务 |
| 401 | 未登录、会话过期/撤销、账号禁用或凭据版本失效 |
| 422 | 提交政策明确拒绝；包含 smtp_code、stage |
| 502 | SMTP 协议不兼容或响应畸形 |
| 503 | 会话/提交服务或持久化失败，可重试 |
| 504 | 等待持久化超时，结果未知，retryable=false |

提交失败响应包含 `message_id`，方便核对邮件。收到结果未知时提示用户核实投递，
不能自动重试。默认 DirectSubmitter 将发件人的 Sent（`box_type=2`）关联与邮件元数据、
收件人/outbox 放入同一主分片事务；HTTP 202 时副本已提交，共享原邮件 ID/正文，
不会额外复制正文文件。缺少系统 Sent 邮箱或关联写入失败会回滚整个提交。
SMTP/可选 Loopback 适配器仍遵循传统客户端另行 APPEND 的行为。
等待持久化 deadline 取自
`inbound_persist_wait_timeout_ms`；超时不撤销在途持久化。
LoopbackSmtpSubmitter 继续作为可注入的兼容适配器，其 SMTP 协议限制和结果映射
仍由专项测试覆盖，默认 Webmail 不使用该路径。

部署已有库时须已执行 `config/sql/migration_mail_subject_len.sql`，使
`mails.subject` 为 varchar(998)；旧 255 字符列可能拒绝合法的编码主题。

## 验证

```bash
cmake --build build-release -j 8
ctest --test-dir build-release -j 4 --output-on-failure
python3 test/scripts/webmail_send_smoke.py --build-dir build-release
# 可选：仅本地数据库；需 mysql-connector-python 与 bcrypt
.venv/bin/python test/scripts/webmail_send_smoke.py --db-config config/db_config.json
```

`smtp_submitter_test` 使用真实 loopback TCP 脚本对端，验证 AUTH、信封、DATA
点透明、拒绝与结果未知；`webmail_test` 验证鉴权、发送者绑定和 MIME 读回。
Python 的 email 标准库独立验证编码。带 `--db-config` 的烟测会使用临时高端口
启动两个 Webmail 与一个 SMTP 服务、创建随机账号、验证共享会话、新登录互踢/并发单会话、重启、
跨节点提交/读信、别名与跨协议配额、注销、状态/到期/改密，
随后清理自身数据和进程。还验证 Sent 与接收邮件共享 ID、Sent 不改变收件人未读、
标记幂等及越权拒绝。完整真实浏览器验收见前端 README 的 fixture 命令。
