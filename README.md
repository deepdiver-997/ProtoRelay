# Mail System (learning project)

This repository contains a small learning SMTP/IMAP server implementation in C++ using Boost.Asio and OpenSSL. It is a study project and not production-ready.

## Overview

- Language: C++17
- Network: Boost.Asio
- TLS: OpenSSL
- Threading: custom thread pool implementations (Boost-based and IO-based)
- Database: MySQL client is referenced but database persistence is not fully implemented (see notes below)

## Build with CMake (recommended)

From project root (macOS, zsh):

```bash
# create build directory and configure
cmake -S . -B build
# build only the SMTP server (or build both targets)
cmake --build build --target smtpsServer -j2
# or build both
cmake --build build --target imapsServer -j2
```

By default this CMake config places intermediate build artifacts in the `build/` directory and the final executables in the project's `test/` directory (e.g. `test/smtpsServer`).

If you prefer the old Makefile, there is a `build/MakeFile` preserved, but CMake is preferred.

## Running the SMTPS server

1. Make sure you have TLS certificate and key files available. The project `.gitignore` excludes `*.crt` and `*.key` files; by default example certs may be in `src/mail_system/back/crt/` if present. Adjust configuration files accordingly.

2. Run the server (default configured port is 465 for SMTPS in the example):

```bash
./test/smtpsServer
```

3. In another terminal, test with OpenSSL's `s_client`:

```bash
openssl s_client -crlf -connect 127.0.0.1:465
```

You can then perform a manual SMTP session. Example session transcript (what you should type and expected responses):

Client -> Server sequence (example):

```
220 SMTPS Server Ready
helo server
250-server Hello
250-SIZE 10240000
250-8BITMIME
250 SMTPUTF8
mail from: <xxx@abc.com>
250 Ok
rcpt to: <xx@123.com>
250 Ok
data
354 Start mail input; end with <CRLF>.<CRLF>
morning my friend!
.
250 Message accepted for delivery
quit
221 Bye
closed
```

> Note: This project is for learning purposes. Database storage/persistence is not fully implemented, so messages are not reliably saved to a database yet. The server may accept messages but they will not be stored in MySQL until DB integration is completed.

## Third-party libraries and license notes

- This project includes the single-header version of nlohmann/json (in `OuterLib/json/single_include`) for convenient JSON parsing. nlohmann/json is distributed under the MIT License. We include it here as a copy of the upstream single-header implementation for educational purposes.

Attribution:

- nlohmann/json — JSON for Modern C++ (single-header)
	- Repository: https://github.com/nlohmann/json
	- License: MIT License

If you redistribute this project or produce a binary including nlohmann/json, you must keep the MIT license text available (the upstream license permits redistribution with attribution). The included single-header comes with its MIT license; ensure you preserve that file's license header if you vendor/update it.

## Configuration

Configuration is read from JSON files in `config/` (e.g. `smtpsConfig.json`, `imapsConfig.json`, `db_config.json`). Adjust address, ports, and certificate paths there before running.

## Troubleshooting

- If you see Boost placeholders deprecation warnings, they do not prevent compilation but can be fixed by changing Boost includes to `boost/bind/bind.hpp` and using `boost::placeholders`.
- If the server seems to hang during startup because DB connection is not available, recent code changes perform DB pool creation asynchronously with a timeout to avoid long blocking in the constructor.

## License & Safety

This is a personal/learning project. Do not use in production. Be cautious with TLS keys and credentials; keep them out of version control (they are ignored by `.gitignore`).

