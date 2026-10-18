# Webmail HTTP API

`webmailServer` 是复用邮件引擎的第四个协议前端。当前支持 cookie 登录会话、
邮箱列表、邮件分页、MIME 正文读取与清洗、通过共享 MailSubmissionService 写信。
附件下载、附件上传、搜索、标记已读、文件夹管理、自动保存发件箱副本和 SPA 尚未完成。

## 启动与装配

```bash
./build-release/webmailServer --help
./build-release/webmailServer config/webmailConfig.json 8081 ./config/webmail
```

参数顺序为配置文件、HTTP 端口、静态根目录。`webmail_node_id` 为 0..1023
的写节点编号，默认 1；所有写同一邮件库的节点须唯一，避开 SMTP 默认 0。
生产配置 `use_database=true`，在 shard 0 应用 `migration_web_sessions.sql`。
原 `webmail_submission_port` 不再读取。Webmail 自己装配配额、外域开关、存储和
outbound 配置，须与 SMTP 一致。详见
[共享会话与提交服务](webmail-sessions-and-submission.md)。
TLS/Secure cookie 感知装配仍待补；静态目录不能指向邮件正文存储。

## 端点

| 方法与路径 | 行为 |
| --- | --- |
| `GET /api/health` | 存活探针 |
| `POST /api/login` | `{address, password}` → HttpOnly、SameSite=Lax cookie |
| `POST /api/logout` | 删除会话并清 cookie |
| `GET /api/me` | 当前用户 id/address |
| `GET /api/mailboxes` | 本人邮箱及 total/unread |
| `GET /api/mailboxes/{id}/messages?page=1&page_size=50` | 邮件分页；每页上限 100 |
| `GET /api/mailboxes/{id}/messages/{mail_id}` | 邮件元数据、解码正文、附件清单 |
| `POST /api/send` | 生成 MIME，按会话身份提交共享 MailSubmissionService |

邮件列表与详情的 `subject` 解码 UTF-8/ASCII RFC 2047 B/Q encoded-word；
未知 charset 保留原文。HTML 正文经过白名单清洗；前端仍须使用无脚本、
无 same-origin 权限的 sandbox iframe 和 CSP，远程图片默认禁用。

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

生产会话使用 shard 0 的 `web_sessions` 表：一次登录一行，保存 token 摘要，
支持多个设备、多个 Webmail 节点和服务重启；logout 只撤销当前会话。
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
不能自动重试。当前不自动生成 Sent 副本。等待持久化 deadline 取自
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
启动两个 Webmail 与一个 SMTP 服务、创建随机账号、验证共享会话、重启、
跨节点提交/读信、别名与跨协议配额、注销、状态/到期/改密，
随后清理自身数据和进程。
