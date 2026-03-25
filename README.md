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

## License

MIT. Boost license in `COPYING_BOOST.txt`.
