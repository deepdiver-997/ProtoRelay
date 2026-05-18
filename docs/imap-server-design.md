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
| LOGIN | ✅ Complete | RFC 3501 §6.2.3 |
| LOGOUT | ✅ Complete | RFC 3501 §6.1.5 |
| SELECT | ✅ Complete | RFC 3501 §6.3.1 |
| EXAMINE | ✅ Complete | RFC 3501 §6.3.2 |
| LIST | ✅ Complete | RFC 3501 §6.3.8 |
| LSUB | ✅ Complete | RFC 3501 §6.3.9 |
| STATUS | ✅ Complete | RFC 3501 §6.3.10 |
| FETCH | ✅ Complete | RFC 3501 §6.4.5 |
| STORE | ✅ Complete | RFC 3501 §6.4.6 |
| EXPUNGE | ✅ Complete | RFC 3501 §6.4.3 |
| CLOSE | ✅ Complete | RFC 3501 §6.4.2 |
| SEARCH | ✅ Basic | RFC 3501 §6.4.4 |
| UID | ✅ Basic | RFC 3501 §6.4.8 |
| NOOP | ✅ Complete | RFC 3501 §6.1.2 |
| CHECK | ✅ Complete | RFC 3501 §6.4.1 |
| CREATE | ✅ Complete | RFC 3501 §6.3.3 |
| DELETE | ✅ Complete | RFC 3501 §6.3.4 |
| RENAME | ✅ Complete | RFC 3501 §6.3.5 |
| SUBSCRIBE | ✅ Stub | RFC 3501 §6.3.6 |
| UNSUBSCRIBE | ✅ Stub | RFC 3501 §6.3.7 |
| IDLE | ✅ Complete | RFC 2177 |
| APPEND | 🔧 Parsed only | RFC 3501 §6.3.11 |
| COPY | 🔧 Stub | RFC 3501 §6.4.7 |
| MOVE | 🔧 Stub | RFC 6851 |

### FETCH Attributes Supported

- `FLAGS` — maps \Seen (mail_recipients.status), \Flagged (is_starred), \Deleted (is_deleted)
- `INTERNALDATE` — send_time formatted per RFC 3501 date-time
- `RFC822.SIZE` — mail body file size
- `ENVELOPE` — date, subject, from, sender, reply-to, to, cc, bcc, in-reply-to, message-id
- `BODY[]` — full message body (read from storage)

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
`UIDVALIDITY` = `mailbox_id`. `UIDNEXT` = `MAX(mail_id) + 1` in the mailbox.

## Configuration

### imapsConfig.json

```json
{
  "address": "0.0.0.0",
  "enable_ssl": true,
  "ssl_port": 993,
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
- **No persistent queue settings** — IMAP reads, doesn't write through the pipeline
- **No inbound auth policy** — IMAP always requires authentication
- **Longer timeouts** — IMAP sessions last longer (30 min idle vs 5 min SMTP)

## Integration

To start the IMAP server alongside the existing SMTP server:

```cpp
#include "mail_system/back/mailServer/imaps_server.h"

// In your main() after SMTP server:
ServerConfig imap_cfg;
imap_cfg.loadFromFile("config/imapsConfig.json");

// Share the same thread pools & DB pool for efficiency
auto imap_server = std::make_shared<ImapsServer>(
    imap_cfg,
    io_pool,     // shared IO thread pool
    work_pool,   // shared worker thread pool
    db_pool      // shared DB pool
);
imap_server->start();
```

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

## Known Limitations (Phase 1)

1. **APPEND** — Literal parsing is wired but storage is not implemented
2. **COPY/MOVE** — Return NO, not yet wired to the database
3. **SEARCH** — Simplified: returns all messages (no search expression filtering)
4. **IDLE** — Accepts DONE correctly, but doesn't push new-mail notifications
5. **STARTTLS** — Advertised in CAPABILITY but handler not wired in the session
6. **BODYSTRUCTURE** — Not yet implemented (only BODY[] works)
7. **INTERNALDATE** — Uses send_time; should use the Date header when available
8. **UID** — Uses mail_id directly; proper per-mailbox sequential UIDs are a future enhancement

## Future Roadmap

- **Phase 2**: APPEND, COPY, MOVE, full SEARCH, STARTTLS
- **Phase 3**: BODYSTRUCTURE, IDLE push notifications, ACL, QUOTA
- **Phase 4**: SORT, THREAD, LIST-EXTENDED, MULTISEARCH
