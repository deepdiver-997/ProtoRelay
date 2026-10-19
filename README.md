# ProtoRelay

ProtoRelay is a C++17 mail system with SMTP/SMTPS, IMAP, POP3, asynchronous outbound delivery, and a Webmail HTTP API with a React frontend.

It also ships two demo/validation servers built on the same framework — a static‑file **HTTP/1.1** server and a **HTTP/2** multi‑stream server (`webServer`, `h2web`, implemented in `framework/http/`). These exist to validate framework reuse and exercise protocol layering, not as production web hosts.

## Framework / Application Separation

The codebase is structured as a reusable framework + application:

```
include/framework/          # 可复用框架（与邮件协议无关）
├── server_base.h           #   服务器生命周期、配置、指标；外部注入资源
├── tcp_server_base.h       #   TCP/SSL 多协议监听器模板
├── session_base.h          #   异步读写 + FSM 集成 + 超时管理
├── fsm_base.h              #   通用状态机 (std::map dispatch)
├── fast_fsm_base.h         #   编译期数组 O(1) dispatch 变体
├── connection/             #   IConnection 抽象 + TCP/SSL 实现
├── db/                     #   DB/连接池抽象接口、配置和 RAII 租约
├── storage/                #   存储 provider、读写流及有界下载流
├── cache/                  #   通用 LruCache
├── http/                   #   HTTP/1.1 与 HTTP/2 组件
├── thread_pool/            #   IO + Worker 线程池
├── metrics_server.h        #   嵌入式 Prometheus 指标
└── intrusion_detector.h    #   基于 IP 的失败尝试跟踪与封禁

include/mail_system/back/   # 邮件系统（框架的成熟应用）
├── mailServer/             #   SMTP/IMAP/POP3 FSM + outbound 调度
├── db/                     #   MySQL/MariaDB 驱动、具体连接池和邮件 SQL
├── storage/                #   通用存储接口/实现的兼容 shim
├── smtp_client/            #   Outbound/兼容提交器共用 SMTP 客户端内核
├── persist_storage/        #   持久化队列 + Bloom 去重
├── inbound/                #   SPF/DKIM/DMARC 入站验证
└── ...

include/framework/http/    # HTTP/1.1 + HTTP/2 协议组件
include/mail_system/back/webmail/ # 会话、HTTP API、提交/阅读编排
webmail/                   # React/TypeScript SPA；构建产物 build-webmail/
```

邮件系统目前是框架上最成熟的应用。框架组件可在其他协议服务中复用；Web 服务器
（`framework/http/`）是验证"框架复用 + 协议分层"的第二应用，详阅 [`docs/web-server/`](docs/web-server/README.md)。

## Current Implemented Scope

The implemented mail protocols cover authenticated submission, inbound MIME/attachment parsing,
IMAP/POP3 mailbox access, and transactional persistence/outbox delivery. Protocol coverage is
practical rather than complete RFC conformance; see each protocol design for current limits.
Outbound runs inside `smtpsServer` (and the default DB-backed `webmailServer`), with DB leases
coordinating workers. There is no separate outbound executable; a relay host runs its own SMTP process.

In parallel, two **web scaffold** servers validate that the same framework (async SessionBase,
FastFsm, IO/worker pools, watchdog) carries a second protocol family:
- `webServer` — HTTP/1.1 static files
- `h2web` — HTTP/2 multi‑stream static files (HPACK + flow control)
Business docs: [`docs/web-server/README.md`](docs/web-server/README.md).

`webmailServer` supports a shared DB session per account (new logins revoke previous cookies), mailbox/message lists, MIME reading, and durable submission through an injected MailSubmissionService (text/HTML, To/Cc). Apply `config/sql/migration_web_sessions.sql` on shard 0 (existing multi-session tables also need `migration_web_sessions_single_login.sql` after stopping old Webmail writers) and give every writer a distinct `webmail_node_id` (0..1023; default 1, SMTP defaults to 0). The React/TypeScript SPA includes login, responsive mailbox/read/compose views, read/star flags, authenticated streaming attachment downloads (local/distributed), and an atomic Sent copy on direct submission. Attachment uploads, server drafts, folder mutations, and full-mailbox search remain pending. Build/start instructions are in [webmail/README.md](webmail/README.md). See the [Webmail API contract](docs/mail-system/architecture/webmail-api.md) for configuration and verification commands.

Current integration limits: SMTP preserves complete MIME attachments for SMTP/IMAP delivery,
but does not yet populate complete standalone attachment objects/indexes for Webmail downloads.
DATA_END DKIM/DMARC computation is not yet connected to hard rejection or stored
Authentication-Results headers. See the [release verification](docs/reports/release-2026-10-18.md)
for tested paths and limits.

## Extensibility by Design

ProtoRelay is structured around replaceable modules instead of monolithic logic:

- Database pool abstraction (`mysql`, `null` for benchmarks)
- Storage provider abstraction (`local`, `null`, `s3`/MinIO, `distributed`, `hdfs_web`)
- Outbound delivery and DNS routing modules
- Config-driven wiring in server bootstrap

This means new providers and strategies can be added with minimal impact on SMTP FSM core behavior.

## CLI (Large-Project Style)

ProtoRelay now follows a stable CLI contract similar to mature tools:

- `--help` / `-h`: consistent usage text
- `--version` / `-V`: build-time injected metadata (version, commit, target, compiler)
- `--config` / `-c <path>`: explicit config file path
- Backward compatibility: one positional `config_path` is still supported

Example:

```bash
./build/smtpsServer --help
./build/smtpsServer --version
./build/smtpsServer -c config/smtpsConfig.json
```

Web scaffold targets (no DB / config file needed):
```bash
./build/webServer ./config/web 8080            # HTTP/1.1 静态文件
./build/h2web 8081 ./config/web                # HTTP/2(prior-knowledge) 静态文件
curl http://localhost:8080/                    # H1
curl --http2-prior-knowledge http://localhost:8081/   # H2
```

## Inbound ACK and Persistence Tuning

Inbound SMTP acceptance is now configurable at runtime:

- `inbound_ack_mode=after_persist`: reply `250 OK` only after persistence succeeds.
- `inbound_ack_mode=after_enqueue`: reply `250 OK` as soon as the message is accepted by the persistence queue.

Related tuning fields:

- `inbound_persist_wait_timeout_ms`: max wait time in `after_persist` mode before returning timeout failure.
- `persist_max_inflight_mails`: cap for total owned messages in persistence pipeline.
- `persist_min_available_memory_mb`: reject enqueue below free-memory threshold.
- `persist_min_db_available_connections`: reject enqueue when DB pool is under pressure.

Example:

```json
{
  "inbound_ack_mode": "after_enqueue",
  "inbound_persist_wait_timeout_ms": 5000,
  "persist_max_inflight_mails": 2048,
  "persist_min_available_memory_mb": 256,
  "persist_min_db_available_connections": 1
}
```

Operational note:

- `after_enqueue` improves throughput and tail latency, but `250 OK` no longer guarantees durable persistence.
- `after_persist` is safer for durability, but throughput is bounded by persistence completion latency.
- **Full benchmark matrix (C++ `smtp_client`)**: see `test/bench/bench-report.md`
  - seq+reuse (local file storage, no DB): **18,278 msg/s** @ 8 threads
  - pipe+reuse (null storage + null DB): **72,303 msg/s** @ 32 threads — 纯 FSM+TCP 上限
  - pipe+reuse (real disk + MySQL, refactor前): **12,502 msg/s** @ 32 threads
  - FSM mock (零 I/O, refactor后): **15,913 msg/s** @ 4 threads (旧: 4,127)
  - localhost per-conn limited by ephemeral port pool (~16384); `--local-ips` 绕过
- M2 Pro (12-core) macOS single-machine figures, not a production SLA
- **72303 msg/s 不含任何磁盘/数据库开销**（null storage + null DB），仅 FSM + TCP loopback
- Use `"storage": {"provider": "null"}` + `"use_database": false` for ceiling benchmarks
- C++ `smtp_client` is the primary bench tool; `test/scripts/cl.py` for TLS/AUTH smoke tests

### Performance Tuning Notes

1. **IO pool distribution**: TCP sockets must bind to `IOThreadPool::get_io_context()` (round-robin across N io_contexts), not `ServerBase::get_io_context()` (single listener context). A prior regression routed all TCP connections to one thread, cutting throughput 7×.
2. **Connection reuse**: Benchmark script reuses connections by default. `--per-conn` for realistic per-message connections.
3. **Auth cache**: `LruCache` in SmtpsFsm (TTL 5min, cap 10000) avoids DB queries for repeat auth.
4. **Persistence queue**: bounded `boost::lockfree::queue`, admission/shutdown synchronization, and a condition variable for idle waiting. Completion subscriptions replace worker sleep polling.
5. **Log level**: INFO-level spdlog synchronous stdout writes do cost under high concurrency (5+ lines per message contending on one stdout mutex); benchmark with `warn`. But it is not the throughput ceiling — the real hotspots on the DB path were TLS+blocking-fd degenerating the "async" DB engine into synchronous io-thread execution, and CPS callbacks dropping the pooled connection mid-flight. Both fixed 2026-09-24; see `test/bench/imap/REPORT.md` §6.
6. **SMTP pipelining**: `do_async_read` checks the command buffer first — complete lines are processed immediately (one FSM round-trip each), only falling back to network read when exhausted. Incomplete commands wait for the next TCP chunk.
7. **Per-thread io_context is load-bearing**: if `IOThreadPool` ever makes every thread share one `io_context` (a `vector<shared_ptr>(N, make_shared)` footgun), one connection's read & write completions get scheduled by N threads → H2's continuous multi-frame pumping races shared outbound state (ASan UAF). H1/SMTP/IMAP/POP3 hide it because they write once per request. Each io thread must own a distinct context. See [`docs/web-server/bugfixes/2026-09-02-shared-io-context.md`](docs/web-server/bugfixes/2026-09-02-shared-io-context.md).

## Current Outbound Hot-Dispatch Semantics

The persistence queue now writes `mail_outbox` in the same DB transaction as `mails`, `mail_recipients`, and `attachments`.

- If the local node can immediately own outbound delivery, it inserts those `mail_outbox` rows as locally leased `SENDING`
- After commit, it hands `unique_ptr<mail>` plus the reserved outbox records to the local outbound client
- If local handoff fails, the reservations are released back to `PENDING` so other nodes can claim them

This hot path currently optimizes for local ownership and one less DB claim round. MIME construction still falls back to reading `body_path`, so it is not yet a fully file-free in-memory outbound path.

## Build-Time Version Injection

Version/build metadata is generated during CMake configure and injected into the binary, including:

- semantic version
- git short commit
- build timestamp (UTC)
- target triple-ish info (`OS-ARCH`)
- compiler identity/version
- feature toggles

## Build

```bash
./build.sh Debug
./build.sh Release
# 或只编 web 相关： cmake --build build --target webServer / h2web / h2_web_test
```

The build script auto-creates `build/` and keeps generated CMake artifacts out of source root.

## Optional logging and worker backends

```bash
cmake -S . -B build/standard \
  -DPROTORELAY_LOG_BACKEND=STANDARD \
  -DPROTORELAY_WORKER_BACKEND=STD
cmake --build build/standard --target smtpsServer unit_tests
ctest --test-dir build/standard --output-on-failure
```

Logging backends: `SPDLOG` (default), `STANDARD`, `NONE`. The latter two do not
require spdlog or fmt. All modes accept a user-provided `LogRecord` callback;
see [logging interfaces](docs/build-deploy/logging-guide.md).
Worker backends: `STD` (default) and the original `BOOST` implementation.
`build.sh` accepts the same names as environment variables; pass the same
`PROTORELAY_LOG_BACKEND` to `link.sh` when linking transferred object-only builds.
The standard pool drains on `stop(true)`, discards queued tasks on `stop(false)`
and always joins active tasks. Discarded futures receive `broken_promise`.
Lifecycle calls require an external thread; stopped pools can be restarted.

`IConnection` now exposes project buffers, `std::error_code`, executor and timer
interfaces. Session scheduling/timeouts, persistence waits, POP3 lock sweeps and heartbeats
use these interfaces. Server maintenance obtains schedulers through `IExecutorProvider`.
`TcpServerBase`, the TCP/TLS connection facades and Metrics HTTP now obtain listening
and STARTTLS operations through `IServerTransportProvider`. Contexts, work guards,
acceptors and SSL contexts live inside the Asio backend; protocol session factories
keep their existing connection types. SMTP/IMAP close timers also use `ITimer`.

Build the framework contracts without Boost, OpenSSL, spdlog, c-ares or MySQL:

```bash
cmake -S . -B build/core -DPROTORELAY_BUILD_BACKEND_CONTRACTS_ONLY=ON
cmake --build build/core
ctest --test-dir build/core --output-on-failure
```

This compiles the real server base, logging and standard worker pool and exercises
server admission, STARTTLS handoff and Metrics HTTP with a deterministic custom backend.
It requires only a C++17 toolchain, Threads and nlohmann_json. The production network
backend, outbound clients and database asynchronous waits still use Asio;
a complete Asio-free mail service is not yet supported.

## Runtime Prerequisites (Summary)

- Linux/macOS
- CMake 3.10+
- GCC 9+ / Clang 10+
- Boost, OpenSSL, MySQL client, c-ares; spdlog is optional (used by the default log backend)
- If `hdfs_web` storage is enabled: also require libcurl

## Project Conventions

See style and engineering conventions:

- `docs/style/PROJECT_STYLE.md`

## A Note for Students

If you're a student looking for a course project reference — feel free to study, borrow, or adapt any part of this codebase. No need to ask. That said, a GitHub star would be much appreciated. Credit where it's due, but mostly it helps me know this project was useful to someone.

## License

MIT. See `LICENSE`. Boost license in `COPYING_BOOST.txt`.
