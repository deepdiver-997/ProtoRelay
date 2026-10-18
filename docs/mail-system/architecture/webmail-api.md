# Webmail HTTP API

`webmailServer` 是复用邮件引擎的第四个协议前端。当前支持 cookie 登录会话、
邮箱列表、邮件分页、MIME 正文读取与清洗、通过本机 SMTP submit 写信。
附件下载、附件上传、搜索、标记已读、文件夹管理、自动保存发件箱副本和 SPA 尚未完成。

## 启动与装配

```bash
./build-release/webmailServer --help
./build-release/webmailServer config/webmailConfig.json 8081 ./config/webmail
```

参数顺序为配置文件、HTTP 端口、静态根目录。`webmail_submission_port` 配置
默认 587，允许 1..65535 的整数；SMTP 目标始终为 `127.0.0.1`，不解析 DNS，
不接受请求指定上游地址。可在没有低端口绑定权限的开发环境中指向本机高端口
SMTP listener；该 listener 必须开启 AUTH 并提供 PLAIN。配置由 webmail 入口
装配，框架配置与静态 HTTP demo 不需要知道提交端口。

静态根目录不能指向邮件或附件存储。数据库、存储与 SMTP 服务共享现有配置；
HTTP 服务没有数据库写邮件的旁路。TLS 终结与 Secure cookie 的装配仍待完成。

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
| `POST /api/send` | 生成 MIME，按当前用户 AUTH 后提交本机 SMTP |

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

`from` 与未知字段返回 400；发件人与 SMTP AUTH 用户始终取自已认证会话。
`bcc` 当前也返回 400：引擎的 IMAP/读信 API 仍将信封收件人列表用于展示，
在读侧隐私契约完成前不开放密送。附件上传/原始 MIME/自定义邮件头不在此接口中。

登录成功后，SMTP 密码以不可复制的缓冲随进程内会话保留，避免每次发信重新输入。
密码不落盘、不进入 API 响应或日志；注销/淘汰释放会话引用，最后一个在途提交
释放引用时擦除该缓冲。TTL 到期后不再允许使用；当前 LRU 没有定时清扫过期条目，
过期条目可能保留到淘汰/进程退出。每次提交均重新 AUTH，SMTP 返回 535 时删除
该 webmail 会话并清 cookie。改密即时生效仍受 SMTP AuthCache TTL 窗口约束。

## 提交结果

成功只在 DATA 最终得到 250 后返回：

```json
{"status":"accepted","message_id":"<random@example.com>"}
```

HTTP 状态为 **202**，表示本机提交服务接受；持久化时点遵循 SMTP 的
`inbound_ack_mode` 配置，不表示外域收件人已经收到。配额、发件人约束、持久化、
投递与 DKIM 继续由现有 SMTP 链处理。客户端不会自行重试。

| HTTP 状态 | 含义 |
| --- | --- |
| 400 / 413 / 415 | 请求校验失败 / 超限 / 媒体类型错误，未提交 SMTP |
| 401 | 未登录、会话过期或 SMTP 验密失败；后者清 cookie |
| 422 | SMTP 5xx 明确拒绝；包含 smtp_code、stage |
| 502 | SMTP 协议不兼容或响应畸形 |
| 503 | 提交前 I/O 故障或 SMTP 4xx 明确拒绝，可重试 |
| 504 | DATA 开始后连接丢失/超时，结果未知，retryable=false |

提交失败响应包含 `message_id`，方便核对邮件。收到结果未知时提示用户核实投递，
不能自动重试。SMTP 提交本身不生成发件箱副本（传统客户端另用 IMAP APPEND），
自动保存副本是后续工作。多收件人中任一 RCPT 被拒绝就断开，不进入 DATA，
避免只给部分收件人发送。一次提交总 deadline 为 30 秒，SMTP 响应最多 64 KiB；
socket/timer 均在同一个 strand 上操作，不阻塞 HTTP 或 worker 线程。

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
启动真实 SMTP/webmail 服务、创建随机账号、完成提交/配额/To/Cc 收件箱读信，
随后清理自身数据和进程。
