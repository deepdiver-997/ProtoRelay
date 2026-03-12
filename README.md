# Mail System V7 - SMTPS Server

A modern C++20 SMTP/SMTPS server with async networking, file-backed body storage, and MySQL persistence.

## Quick Start

### Prerequisites
- Linux or macOS
- GCC 9+ / Clang 10+ with C++20
- CMake 3.10+
- Dependencies: Boost.Asio, OpenSSL, MySQL client, spdlog

### Install deps (macOS/Homebrew)
```bash
brew install boost openssl mysql-client spdlog
export OPENSSL_ROOT_DIR=$(brew --prefix openssl)
export MYSQL_ROOT_DIR=$(brew --prefix mysql-client)
```

### Install deps (Ubuntu/Debian)
```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake libboost-all-dev libssl-dev libmysqlclient-dev libspdlog-dev
```

### Build
Fast path:
```bash
./build.sh Debug    # dev
./build.sh Release  # perf
```
Manual:
```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```
Binary output: `test/smtpsServer`.

## Configuration

### Database (`config/db_config.json`)
Key fields:
```json
{
  "achieve": "mysql",
  "host": "localhost",
  "user": "mail_test",
  "password": "<password>",
  "database": "mail",
  "initialize_script": "sql/create_tables.sql",
  "port": 3306,
  "initial_pool_size": 32,
  "max_pool_size": 128,
  "connection_timeout": 5,
  "idle_timeout": 300
}
```
Create DB and user:
```sql
CREATE DATABASE mail CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
CREATE USER 'mail_test'@'localhost' IDENTIFIED BY '<password>';
GRANT ALL PRIVILEGES ON mail.* TO 'mail_test'@'localhost';
FLUSH PRIVILEGES;
USE mail;
SOURCE sql/create_tables.sql;
```

### Server (`config/smtpsConfig.json`)
Important knobs:
```json
{
  "address": "0.0.0.0",
  "ssl_port": 465,
  "tcp_port": 25,
  "enable_ssl": true,
  "enable_tcp": true,
  "io_thread_count": 24,
  "worker_thread_count": 12,
  "use_database": true,
  "db_config_file": "db_config.json",
  "maxMessageSize": 1048576,
  "log_level": "info",
  "log_file": "../logs/server.log",
  "mail_storage_path": "../mail/",
  "attachment_storage_path": "../attachments/"
}
```

### Outbound Deliverability Baseline (2026-03)
Recommended settings for direct MTA-to-MTA delivery:

```json
{
  "system_domain": "mail.hgmail.xin",
  "outbound_helo_domain": "mail.hgmail.xin",
  "outbound_mail_from_domain": "mail.hgmail.xin",
  "outbound_dkim_enabled": true,
  "outbound_dkim_selector": "default",
  "outbound_dkim_domain": "mail.hgmail.xin",
  "outbound_ports": [25]
}
```

Notes:
- For internet MX delivery, use port 25 and upgrade with STARTTLS when offered.
- Ports 465/587 are typically submission ports, not the default path for MTA-to-MTA relay.
- DNS alignment checklist: SPF + DKIM + PTR (+ DMARC).

Quick DNS checks:
```bash
dig +short TXT mail.hgmail.xin
dig +short TXT default._domainkey.mail.hgmail.xin
dig +short TXT _dmarc.mail.hgmail.xin
dig +short -x 8.134.123.121
```

### Demo Video
QQ -> Mail System delivery demo recording:

 External demo link (Release/Object Storage): `<replace-with-public-url>`

### TLS
mkdir -p config/crt
openssl req -x509 -newkey rsa:4096 -keyout config/crt/server.key \
    -out config/crt/server.crt -days 365 -nodes -subj "/CN=localhost"
```

## Run
```bash
mkdir -p logs mail attachments
./build/test/smtpsServer   # or from build dir: ./test/smtpsServer
```
Tail logs:
```bash
tail -f logs/mail_system.log
```

## Testing
Python stress script:
```bash
uv run ./test/cl.py --messages 300
```
Or swaks:
```bash
swaks --to dest@example.net --from sender@example.com --server localhost:25 --body "Hello"
```

## Performance (MacBook Pro M2, Release)
- io_thread_count: 24, worker_thread_count: 12
- MySQL pool: initial 32, max 128
- ~180 messages/sec sustained for 300–600 small messages (no auth, small bodies)

## Tuning Pointers
- Match threads to cores (IO ~50–75% of cores; worker ~75–150%).
- Keep mail/attachment storage on fast SSD/NVMe; set `ulimit -n` high.
- MySQL: raise `max_connections`, tune InnoDB (`innodb_buffer_pool_size`, `innodb_log_file_size`, `innodb_flush_log_at_trx_commit=2`).
- For production: require AUTH, enforce TLS, add spam/abuse protections.

## Project Layout
- `config/` configs and certs
- `include/` headers
- `src/` sources
- `test/` entry binaries and client scripts
- `docs/` architecture and guides
- `mail/`, `attachments/`, `logs/` runtime data

## License
MIT. Boost license in `COPYING_BOOST.txt`; third-party notices in docs.
