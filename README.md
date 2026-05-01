# ProtoRelay

ProtoRelay is a C++20 mail relay core focused on SMTP protocol execution and delivery pipeline foundations.

## Current Implemented Scope

At this stage, ProtoRelay intentionally focuses on core SMTP capabilities:

- SMTP state machine (session lifecycle and command flow)
- SMTP parsing (commands, message body handling, envelope flow)
- Delivery pipeline (queue + outbound relay path)

This is a deliberate narrow scope: the project is building a robust relay core before adding broader protocol surface.

## Extensibility by Design

ProtoRelay is structured around replaceable modules instead of monolithic logic:

- Database pool abstraction (`mysql`, `mysql_distributed`)
- Storage provider abstraction (`local`, `distributed`, `hdfs_web`)
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
./test/smtpsServer --help
./test/smtpsServer --version
./test/smtpsServer --config config/smtpsConfig.json
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
- Local benchmark note: on this MacBook Pro, `after_enqueue` small-mail tests with `uv run ./test/cl.py` reached roughly 1.9k-3.0k msg/s, with 1000 messages at 2976.9 msg/s and 10000 messages at 2147.5 msg/s under `--concurrency 100`.
- These are single-machine throughput figures under the current test workload, not a production SLA.

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
```

The build script auto-creates `build/` and keeps generated CMake artifacts out of source root.

## Runtime Prerequisites (Summary)

- Linux/macOS
- CMake 3.10+
- GCC 9+ / Clang 10+
- Boost, OpenSSL, MySQL client, spdlog, c-ares
- If `hdfs_web` storage is enabled: also require libcurl

## Project Conventions

See style and engineering conventions:

- `docs/PROJECT_STYLE.md`

## A Note for Students

If you're a student looking for a course project reference — feel free to study, borrow, or adapt any part of this codebase. No need to ask. That said, a GitHub star would be much appreciated. Credit where it's due, but mostly it helps me know this project was useful to someone.

## License

MIT. See `LICENSE`. Boost license in `COPYING_BOOST.txt`.
