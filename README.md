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

## Prerequisites (platform-specific)

This project depends on C++17, CMake, Boost, OpenSSL and a MySQL client library. Below are example installation commands for common platforms. These are suggestions — use your system's package manager or dependency manager as appropriate.

Linux (Ubuntu/Debian):

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config libssl-dev libboost-all-dev libmysqlclient-dev
```

macOS (Homebrew):

```bash
# install brew if not present, then:
brew update
brew install cmake openssl boost mysql pkg-config
# On macOS, CMake may need help finding OpenSSL; see build step below for -D flags
```

Windows (Visual Studio + vcpkg recommended):

1. Install Visual Studio (with "Desktop development with C++").
2. Install vcpkg and use it to install libs:

```powershell
# from a Developer Powershell
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
./bootstrap-vcpkg.bat
./vcpkg install boost openssl mysql-client
# then integrate with cmake by passing -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
```

## Configure and build (examples)

Basic out-of-source configure and build (works on Linux/macOS/Windows with proper deps):

```bash
# from project root
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target smtpsServer -j$(nproc)
```

If CMake cannot find OpenSSL or MySQL include/libs automatically, point it explicitly. Example (macOS Homebrew OpenSSL):

```bash
cmake -S . -B build -DOPENSSL_ROOT_DIR=$(brew --prefix openssl) -DMYSQL_INCLUDE_DIRS=/usr/local/opt/mysql/include -DMYSQL_LIBRARIES=/usr/local/opt/mysql/lib/libmysqlclient.dylib
```

If Boost is installed in a non-standard prefix, set BOOST_ROOT or BOOST_INCLUDEDIR / BOOST_LIBRARYDIR:

```bash
cmake -S . -B build -DBOOST_ROOT=/path/to/boost
```

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

You can then perform a manual SMTP session. See the original README for an example session transcript.

## Third-party libraries and license notes

- This project includes the single-header version of nlohmann/json (in `OuterLib/json`) for convenient JSON parsing. nlohmann/json is distributed under the MIT License. We include it here as a copy of the upstream single-header implementation for educational purposes.

Attribution:

- nlohmann/json — JSON for Modern C++ (single-header)
    - Repository: https://github.com/nlohmann/json
    - License: MIT License

## Notes about the Boost license

This project links against Boost. If you redistribute binaries or vendor Boost headers, include the Boost Software License text. A copy is provided in `COPYING_BOOST.txt` in the project root.

## OpenSSL license and notes

This project links to OpenSSL. OpenSSL's licensing depends on the version:

- OpenSSL 1.1.x and earlier: OpenSSL License + SSLeay License (see upstream for exact text).
- OpenSSL 3.x: Apache License 2.0.

If you redistribute OpenSSL with your binaries, ensure you comply with the applicable OpenSSL licensing terms and include its license files and attribution. For redistribution, include the OpenSSL license text that came with your OpenSSL installation, or reference the upstream license page:

https://www.openssl.org/source/license.html

macOS/Homebrew note: if you install OpenSSL via Homebrew, it typically lives under `/opt/homebrew` (Apple Silicon) or `/usr/local/opt/openssl@1.1` (Intel). You can help CMake find it by passing:

```bash
cmake -S . -B build -DOPENSSL_ROOT_DIR=$(brew --prefix openssl)
```

For MySQL on macOS/Homebrew a common set of flags is:

```bash
cmake -S . -B build -DMYSQL_INCLUDE_DIRS=/opt/homebrew/include -DMYSQL_LIBRARIES=/opt/homebrew/lib/libmysqlclient.dylib
```

## Configuration

Configuration is read from JSON files in `config/` (e.g. `smtpsConfig.json`, `imapsConfig.json`, `db_config.json`). Adjust address, ports, and certificate paths there before running.

## Troubleshooting

- If CMake warns about FindBoost deprecation policies on newer CMake, you can set a policy or install Boost via a package manager/vcpkg that provides CMake targets.
- If MySQL client libs are not found, install `pkg-config` and `libmysqlclient` (or set `MYSQL_INCLUDE_DIRS` / `MYSQL_LIBRARIES` manually when invoking cmake as shown above).
- On macOS with M1/Apple Silicon, Homebrew default prefix may be `/opt/homebrew` — adapt the `-D` paths accordingly.

## License & Safety

This is a personal/learning project. Do not use in production. Be cautious with TLS keys and credentials; keep them out of version control (they are ignored by `.gitignore`).
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

