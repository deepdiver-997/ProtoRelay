# IMAP4rev1 Server Implementation

## Overview

The IMAP4rev1 (RFC 3501) server is a new protocol handler added alongside the existing SMTP server.
It shares the same infrastructure — `ServerBase`, `SessionBase`, `IConnection`/`TcpConnection`/`SslConnection`,
database connection pool, storage provider, and logger — while implementing a parallel FSM-based
protocol state machine.

```
┌──────────────────────────────────────────────────┐
│                  ServerBase                      │
│ (acceptor loop, SSL/TCP, metrics, connection mgt)│
├─────────────────┬────────────────────────────────┤
│  SmtpsServer    │       ImapsServer              │
│  (SMTP FSM)     │       (IMAP FSM)               │
└─────────────────┴────────────────────────────────┘
```

## Architecture

### File Layout

```
include/mail_system/back/mailServer/
├── fsm/imaps/
│   ├── imaps_fsm.hpp               ← ImapState/ImapEvent enums, ImapContext, base class + DB helpers
│   ├── traditional_imaps_fsm.h     ← TraditionalImapsFsm declaration
│   └── traditional_imaps_fsm.tpp   ← Transition table + all command handlers
├── session/
│   ├── imaps_session.h             ← ImapsSession declaration
│   └── imaps_session.tpp           ← IMAP command parser, literal handling
├── imaps_server.h                  ← ImapsServer declaration
src/mail_system/back/mailServer/
└── imaps/imaps_server.cpp          ← Accept loop, session creation
```

### State Machine

IMAP protocol states (RFC 3501 §3):

```
INIT ──(CONNECT)──▶ NOT_AUTHENTICATED
                        │
                   (LOGIN success)
                        │
                        ▼
                  AUTHENTICATED
                        │
                   (SELECT/EXAMINE)
                        │
                        ▼
                    SELECTED
                        │
                   (CLOSE / LOGOUT)
                        ▼
                  AUTHENTICATED  or  LOGOUT
```

### Supported Commands (Phase 1)

| Command | Status | RFC Reference |
|---------|--------|--------------|
| CAPABILITY | ✅ Complete | RFC 3501 §6.1.1 |
| LOGIN | ✅ Complete（bcrypt 已挪 worker 线程，见下） | RFC 3501 §6.2.3 |
| LOGOUT | ✅ Complete | RFC 3501 §6.1.5 |
| SELECT | ✅ Complete | RFC 3501 §6.3.1 |
| EXAMINE | ✅ Complete | RFC 3501 §6.3.2 |
| LIST | ✅ Complete | RFC 3501 §6.3.8 |
| LSUB | ✅ Complete | RFC 3501 §6.3.9 |
| STATUS | ✅ Complete | RFC 3501 §6.3.10 |
| FETCH |（读路径已异步化：续作链 + provider async，见 storage-abstraction.md） ✅ Complete | RFC 3501 §6.4.5 |
| STORE | ✅ Complete | RFC 3501 §6.4.6 |
| EXPUNGE | ✅ Complete | RFC 3501 §6.4.3 |
| CLOSE | ✅ Complete | RFC 3501 §6.4.2 |
| SEARCH | ✅ 支持常用过滤 | RFC 3501 §6.4.4 |
| UID | ✅ Basic | RFC 3501 §6.4.8 |
| NOOP | ✅ Complete | RFC 3501 §6.1.2 |
| CHECK | ✅ Complete | RFC 3501 §6.4.1 |
| CREATE | ✅ Complete | RFC 3501 §6.3.3 |
| DELETE | ✅ Complete | RFC 3501 §6.3.4 |
| RENAME | ✅ Complete | RFC 3501 §6.3.5 |
| SUBSCRIBE | ✅ Stub | RFC 3501 §6.3.6 |
| UNSUBSCRIBE | ✅ Stub | RFC 3501 §6.3.7 |
| IDLE | ✅ 可进出，无推送 | RFC 2177 |
| STARTTLS | ✅ 已实现 | RFC 3501 §6.2.1 |
| APPEND | ✅ 完整实现 | RFC 3501 §6.3.11 |
| COPY | ✅ 完整实现 | RFC 3501 §6.4.7 |
| MOVE | ✅ 完整实现 | RFC 6851 |

### FETCH Attributes Supported

- `FLAGS` — maps \Seen (mail_recipients.status), \Flagged (is_starred), \Deleted (is_deleted)
- `INTERNALDATE` — send_time formatted per RFC 3501 date-time
- `RFC822.SIZE` — mail body file size
- `ENVELOPE` — date, subject, from, sender, reply-to, to, cc, bcc, in-reply-to, message-id
- `BODY[]` — full message body (read from storage)

## APPEND / SEARCH correctness (2026-10-21)

### APPEND 完成后的 session 续读

生产表现是客户端已收到 `OK [APPENDUID ...]`，同连接后续 SELECT、LOGOUT
都没有处理日志。根因在 session 读链：imaplib 可以把 literal 和命令终结 CRLF
分开放进 TLS record；APPEND 完成后启动的读只读到 `\r\n`，解析器把它作为空行
忽略，而 `SessionBase` 认为已经分发过输入，等待不存在的响应完成回调继续读取。
同理，literal 未收齐的一块数据也没有响应可用于推进下一次读取。

`ImapsSession::handle_read()` 现在在忽略空行或 literal 尚未收齐时主动续读。
`SessionBase::do_async_read()` 在连接 executor 上领取一个在途读标志，完成时释放，
使空行、写完成和暂存响应 flush 的多个续读来源不会重叠读取同一 socket/SSL stream。
既有 `pending_write_buf_` 排空冲刷仍保留；该冲刷的默认完成回调原本就会续读，
并非这次“APPEND OK 之后挂死”的根因。

回归 `imaps_fsm_test::append_success_then_select_responds` 从 MockConnection 的
分块读取入口驱动完整 session，经过真实 LOGIN、异步 worker APPEND、SELECT、LOGOUT，
不再直呼 `handle_read/process_read` 绕过读重武装。覆盖同段尾符、OK 后才到达的独立
CRLF、literal 分片、CR/LF 分片和延迟写完成。`session_base_test` 另验证多次续读请求
只发起一个底层读，以及读完成后可以正常发起下次读。

### SEARCH HEADER 零命中与 APPEND 元数据路径

只读核查阿里云 2026-10-21 的 sent-copy 探针：原始头为明文
`Subject: sent-copy probe`，正文对象存在，但对应 `mails.body_path` 长度为 0。
journal 中的参数实际是 `HEADER Subject sent-copy probe`（没有引号）。
普通带引号多词值在修复前基线已能匹配；把该现象归因于拆词并不准确。

真正的持久化问题在 `create_mail_async()`：同一 `async_execute` 调用既把 `body_path`
放入 SQL 参数，又在回调捕获中 `std::move(body_path)`。C++17 不指定两个参数的求值先后，
GCC-11 可先执行移动捕获，随后把空路径写进 SQL；macOS Clang 的顺序掩盖了该问题。
现在回调复制路径，确保 SQL 参数、provider object key 和成功回调路径一致。
SEARCH 读取历史空路径行时按 mail_id 调用同一个 provider 的 `build_mail_body_key()`
定位正文（无 provider 时用原本的 `mail/<id>` 本地布局），不修改数据库。
这是 SEARCH 的历史数据读取兼容；其他读路径及历史行修复不在本次数据写入范围内。

判据消费另有独立缺陷：HEADER field/value 提取后仍参与 flag 和 UID 范围扫描，
如值 `"UNSEEN"` 或 `"UID 9999:*"` 会附加不相关过滤；空引号值还会被分词器丢弃。
现在只分词一次并消费 HEADER 的两个参数，剩余 token 才解释为 flag/UID 条件，
保留空值（字段存在即命中）、多词、`\"` 和 `\\` 转义。UID SEARCH 使用 UID 子命令参数。
HEADER 仍按字段名/内容大小写不敏感的子串匹配，多条 HEADER 隐式 AND。

回归：`append_metadata_body_path` 断言 SQL path = storage key = callback path，
在 gcc-11 上能捕获旧实现的求值顺序问题；`search_header_criteria` 覆盖多词命中/不命中、
普通 SEARCH 与 UID SEARCH、引号/反斜杠转义、空值、字段缺失、flag/UID 参数隔离、
多 HEADER 的 AND，以及正文存在但路径为空的历史数据。后者在 macOS 修复前也稳定失败。

### 验证结果（2026-10-21）

| 平台 / 构建 | 全量 `test/run_all_tests.sh --no-build` |
|---|---|
| macOS Clang / Debug | 44 通过，0 失败，0 跳过 |
| macOS Clang / Release | 44 通过，0 失败，0 跳过 |
| Ubuntu 22.04.5 amd64 / GCC 11.4 / Release | 43 通过，1 存量失败（`line_folder_test.cpp:158`），0 跳过 |

构建均使用 `./build.sh <Debug|Release>`，无手工并行编译。macOS 与 Linux 的真实
MariaDB 单测均实际执行并通过；Linux 使用容器内独立测试库。Linux 的唯一失败是
本次任务预先确认的 LineFolder 存量问题，未改动该组件。修复前失败日志、构建日志与
完整测试汇总保存在本地 `build/test-logs/imap-fix-20261021/`，逐个测试日志按平台存放。

## Database Schema Mapping

The IMAP server uses the existing database schema **without any modifications**:

```
users ──┬── mailboxes (INBOX, Sent, Trash, etc.)
         │      │
         │      └── mail_mailbox (mail ↔ mailbox mapping + flags)
         │
         └── mail_recipients (mail ↔ recipient, includes read/unread status)
                 │
                 └── mails (subject, body_path, send_time)
```

### IMAP Flag Mapping

| IMAP Flag | Database Field |
|-----------|---------------|
| `\Seen` | `mail_recipients.status` = 0 (read) / 1 (unread) |
| `\Flagged` | `mail_mailbox.is_starred` = 1 |
| `\Deleted` | `mail_mailbox.is_deleted` = 1 |
| `\Draft` | `mail_recipients.status` = 3 |
| `\Answered` | Not yet mapped (future) |
| `\Recent` | Computed: EXISTS - UNSEEN |

### UID Assignment

UID is the Snowflake `mail_id` (globally unique, monotonically increasing).
`UIDVALIDITY` = `mailbox_id`. `UIDNEXT` is a per-mailbox high-watermark kept in
the `mailbox_uidnext` table (see `migration_imap_uidnext.sql`): a row lock +
`GREATEST()` advance makes it atomic and monotonic across concurrent / cross-instance
readers, and prevents regression after expunging the largest mail_id.

## Database Access — Async CPS

All DB access in the FSM goes through **CPS** (`async_query` / `async_execute` +
callback continuations), same pattern as POP3/SMTP. There is no synchronous
query/execute bridge in the IMAP path.

- **Read helpers** (need the result to reply) — async CPS chains:
  `auth_user_async`, `get_mailboxes_async`, `find_mailbox_id_async`,
  `get_mailbox_mails_async`, `get_mailbox_stats_cached_async`,
  `get_mailbox_count/unseen/uidnext_async`, `get_user_email_async`, …
- **Write helpers** (fire-and-forget or awaited before reply) — single statements:
  `create_mail_async`, `link_mail_to_mailbox_async`, `expunge_mailbox_async`, …
- **Bulk loops flattened to one IN-list SQL**: STORE uses
  `batch_mark_seen/deleted/flagged_async`; COPY/MOVE use
  `batch_insert_mailbox_async` (`INSERT IGNORE ... VALUES (...),(...)`, count via
  `SELECT ROW_COUNT()`) and `batch_mark_move_deleted_async`.
- The `conn` is a `std::shared_ptr<ScopedConnection>` kept alive through the whole
  chain (each callback captures it). Callers must guard `!conn || !conn->is_valid()`
  (an empty shard router yields an *invalid* ScopedConnection — null `connection_`).

The MySQL `async_*` wrappers currently execute inline (synchronous), but the caller
structure is already async-ready: the plan is to swap the engine to MariaDB
Connector/C (nonblocking, io_context-driven) so io threads stop blocking on the
query. See [`database-async-design.md`](database-async-design.md).

Mailbox stats (SELECT/STATUS) additionally use a **single-flight** dedup so N
concurrent SELECTs of the same mailbox fire only one source query chain. Details in
[`mailbox-concurrency.md`](mailbox-concurrency.md).

## Configuration

### IMAP listener configuration (in smtpsConfig.json)

IMAP 监听器通过主配置文件中的 `listeners` 数组配置：

```json
{
  "listeners": [
    { "type": "imap_ssl",  "port": 993 },
    { "type": "imap_tcp",  "port": 143 }
  ],
  "enable_tcp": true,
  "tcp_port": 143,
  "certFile": "crt/server.crt",
  "keyFile": "crt/server.key",
  "use_database": true,
  "db_config_file": "config/db_config.json",
  "system_domain": "mail.example.com",
  "mail_storage_path": "../mail/",
  "attachment_storage_path": "../attachments/",
  "log_file": "../logs/imap_server.log"
}
```

Key differences from SMTP config:
- **No outbound/DKIM fields** — IMAP doesn't deliver mail
- **No SMTP persist queue** — APPEND/COPY/MOVE write through IMAP DB/storage operations
- **No inbound auth policy** — IMAP always requires authentication
- **Longer timeouts** — IMAP sessions last longer (30 min idle vs 5 min SMTP)

## Integration

独立部署通常分别使用 `config/imapsConfig.json` 和 `config/smtpsConfig.json`，读取同一
权威数据库与可见的正文/附件存储。listener.type 表示 TCP/SSL，不表示 SMTP/IMAP 协议。
合并入口可共享装配资源；独立进程各自装配资源。

```cpp
#include "mail_system/back/bootstrap.h"
#include "mail_system/back/mailServer/imaps_server.h"
mail_system::ServerConfig cfg;
cfg.loadFromFile("config/imapsConfig.json");
auto resources = mail_system::bootstrap::assemble(cfg);
auto imap_server = std::make_shared<ImapsServer>(
    cfg, resources.io_pool, resources.worker_pool, resources.shard_router);
imap_server->start();
```

APPEND/COPY/MOVE 等操作会写数据库/存储；独立 IMAP 不拥有 SMTP 外投队列。
通用接口在 framework，具体 DB 驱动仍由应用装配。

### Testing with openssl

```bash
# IMAPS (SSL, port 993)
openssl s_client -connect localhost:993 -crlf -quiet

# IMAP (STARTTLS, port 143)
openssl s_client -connect localhost:143 -starttls imap -crlf -quiet
```

Once connected, try:

```
. LOGIN user@domain.com password
. LIST "" "*"
. SELECT INBOX
. FETCH 1:* (FLAGS INTERNALDATE RFC822.SIZE ENVELOPE)
. FETCH 1 BODY[]
. STORE 1 +FLAGS (\Seen)
. LOGOUT
```

## Known Limitations (Phase 2)

1. **IDLE 推送** — 能进出 IDLE，但不会主动推送新邮件 EXISTS 通知（需后台轮询机制）
2. **MIME FETCH 已实现** — BODYSTRUCTURE、BODY[]/HEADER/TEXT 与多 MIME section；附件 disposition/文件名、Content-ID 及 RFC 2231 连续参数已补测。MIME sidecar 元数据版本 2，旧版按需从原始 MIME 原子重建。复杂 MIME（含 message/rfc822 嵌套结构）的兼容性仍需扩展，不能宣称完整 RFC 覆盖；见 [手机附件复盘](../../bugfixes/2026-10-18-imap-attachment-bodystructure.md)
3. **INTERNALDATE** — 目前使用 mails.send_time；建议在 Date header 可用时优先使用
4. **UID** — 直接使用 mail_id (Snowflake)；未来可改为每邮箱自增 UID
5. **SEARCH** — 当前为已实现条件子集，不能把它等同于 Webmail 全库全文/语义检索
6. **ACL / QUOTA** — 未实现
7. **SORT / THREAD** — 未实现
8. **LIST-EXTENDED** — 未实现

## Future Roadmap

- **Phase 3**: IDLE 主动通知、ACL、QUOTA；BODYSTRUCTURE 已实现
- **Phase 4**: SORT, THREAD, LIST-EXTENDED, MULTISEARCH, full-text SEARCH
