# ProtoRelay Architecture

## 1. Overview

ProtoRelay is a C++20 SMTP/SMTPS server focused on reliable mail ingest, local persistence, and asynchronous outbound delivery.

Current implementation status:

- Inbound: SMTP protocol parsing, state machine, MIME/attachment handling.
- Persistence: local filesystem, distributed filesystem roots, optional WebHDFS provider.
- Outbound: queue-driven delivery worker with retry/backoff and lease-style claiming.
- Operations: service-manager-first deployment (systemd/launchd), configurable log sinks.

This document reflects the current state of the repository and replaces older V7-era descriptions.

## 2. Design Principles

- Foreground process first: let systemd/launchd/Docker supervise lifecycle.
- Async I/O + bounded workers: separate network loop and business execution.
- Interface-driven storage and DB access: swap implementations with limited blast radius.
- Fail-fast configuration: invalid or unsupported runtime options are rejected at startup.
- Practical operability: explicit build features, clear runtime diagnostics, and flexible logging.

## 3. Layered Architecture

```text
+--------------------------------------------------------------+
| Application Entry                                             |
| test/smtps_test.cpp                                           |
+-------------------------------+------------------------------+
                                |
+-------------------------------v------------------------------+
| Server Core                                                   |
| server_base, smtps server, session management, SMTP FSM      |
+-------------------------------+------------------------------+
                                |
+-------------------------------v------------------------------+
| Message Pipeline                                               |
| parser -> envelope/message model -> persistence -> outbox     |
+-------------------------------+------------------------------+
                                |
+-------------------------------v------------------------------+
| Infrastructure Adapters                                        |
| DB pool/service, storage providers, outbound SMTP client      |
+-------------------------------+------------------------------+
                                |
+-------------------------------v------------------------------+
| Platform                                                        |
| Boost.Asio, OpenSSL, MySQL client, optional libcurl (WebHDFS) |
+--------------------------------------------------------------+
```

## 4. Core Components

## 4.1 Inbound SMTP/SMTPS

Responsibilities:

- Accept SSL and/or TCP listeners based on config.
- Drive SMTP command state transitions.
- Parse headers/body, including MIME multipart attachments.
- Persist mail metadata/content and enqueue outbound tasks.

Key points:

- Connection/session abstractions isolate transport details.
- Session logic is built around async callbacks and strict ownership rules.
- Validation and protocol responses are generated with SMTP utility helpers.

## 4.2 Outbound Delivery Pipeline

Responsibilities:

- Poll outbound queue.
- Claim pending items (lease style) to avoid duplicate processing.
- Resolve destination MX/host policy and attempt SMTP delivery.
- Retry with capped backoff until success or max attempts.

Operational knobs (from server config):

- `outbound_max_attempts`
- `outbound_poll_busy_sleep_ms`
- `outbound_poll_backoff_base_ms`
- `outbound_poll_backoff_max_ms`
- `outbound_poll_backoff_shift_cap`
- `outbound_ports`

Identity knobs:

- `outbound_helo_domain`
- `outbound_mail_from_domain`
- `outbound_rewrite_header_from`
- `outbound_dkim_*`

## 4.3 Storage Provider Abstraction

Runtime selectable via `storage_provider`:

- `local`: standard local directories.
- `distributed`: multiple configured roots with replica count.
- `hdfs_web`: WebHDFS-backed provider.

WebHDFS is also build-time gated:

- CMake option: `ENABLE_HDFS_WEB_STORAGE`
- ON: compile/link WebHDFS provider and libcurl.
- OFF: selecting `hdfs_web` at runtime fails fast with a clear error.

This prevents unnecessary dependency coupling for deployments that do not need HDFS.

## 4.4 Database Access

Responsibilities:

- Connection pool lifecycle and query execution isolation.
- Persist user/mail metadata and queue/outbox related records.
- Support for distributed database pool scenarios where configured.

Reliability practices:

- RAII for connection ownership.
- Startup-time config validation for DB connection settings.
- Error reporting through module-specific log channels.

## 4.5 Logging Subsystem

Built on spdlog with module-oriented loggers.

Runtime controls:

- `log_level`
- `log_to_console`
- `log_to_file`
- `log_file`

Sink strategy:

- Console sink for supervisor capture (journald/launchd stdout/stderr).
- Rotating file sink for standalone or file-centric environments.
- If both sinks are disabled by mistake, logger falls back to console sink.

Recommended production profile under systemd:

- `log_to_console=true`
- `log_to_file=false`

## 5. Concurrency Model

- I/O threads handle socket readiness and async operations.
- Worker threads execute heavier business tasks.
- Outbound delivery runs as a background worker loop with adaptive polling.
- Shared resources (queue/storage/db) are synchronized by adapter internals.

## 6. Configuration and Startup

Primary runtime config is JSON-based and loaded by `ServerConfig`.

Startup sequence (simplified):

1. Parse config file and resolve relative paths.
2. Validate listener, SSL, timeout, storage, and optional HDFS settings.
3. Initialize logger sinks and level.
4. Initialize DB/storage adapters.
5. Start listeners and background outbound loop.

Failure policy:

- Invalid critical config causes immediate startup failure.
- Feature mismatch (for example, `hdfs_web` with build-time OFF) fails early and explicitly.

## 7. Build-Time Feature Model

Important CMake options:

- `ENABLE_HDFS_WEB_STORAGE` (default ON)

Effects:

- Controls whether WebHDFS provider is compiled.
- Controls whether CURL is required and linked.
- Exposed in version/help feature metadata (`+hdfs-web` or `-hdfs-web`).

## 8. Deployment Topology

Supported supervisor patterns:

- Linux: systemd unit template in `deploy/systemd/protorelay.service`
- macOS: launchd plist in `deploy/launchd/io.protorelay.server.plist`

Recommended production shape:

- One ProtoRelay instance per config profile.
- Dedicated writable directories for logs/mail/attachments.
- External MySQL and optional HDFS cluster endpoints.
- TLS certificates provisioned externally (self-signed for dev, CA-issued for prod).

## 9. Current Scope and Extension Points

Implemented now:

- SMTP state machine and message parsing.
- Message persistence and outbound queue processing.
- Configurable storage providers and optional WebHDFS.
- Service-manager-friendly operation.

Expected extension directions:

- IMAP/POP retrieval plane.
- Richer policy engine (rate limits, ACL, anti-abuse hooks).
- Metrics/trace export for observability stacks.
- Certificate hot-reload and rolling restart ergonomics.

## 10. Related Docs

- `README.md`
- `README_zh.md`
- `docs/smtp-outbound-client-design.md`
- `docs/service-deployment.md`
- `docs/domain-deployment-guide.md`
