# Mail System V6 - SMTPS 邮件服务器

一个基于 C++20 的现代化 SMTPS (SMTP over SSL/TLS) 邮件服务器，采用异步网络编程架构，提供高性能、可扩展的邮件收发服务。

## 🚀 快速开始

### 环境要求

- **操作系统**: Linux / macOS
- **编译器**: GCC 9+ / Clang 10+ (支持 C++20)
- **CMake**: 3.10+
- **依赖库**:
  - Boost.Asio (system, thread)
  - OpenSSL (SSL/TLS)
  - MySQL (mysqlclient)
  - spdlog (日志系统)

### 安装依赖

#### macOS (Homebrew)

```bash
# 安装依赖库
brew install boost openssl mysql-client spdlog

# 设置环境变量
export OPENSSL_ROOT_DIR=$(brew --prefix openssl)
export MYSQL_ROOT_DIR=$(brew --prefix mysql-client)
```

#### Linux (Ubuntu/Debian)

```bash
# 安装依赖库
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    libboost-all-dev \
    libssl-dev \
    libmysqlclient-dev \
    libspdlog-dev
```

## 📦 编译项目

### 快速构建（推荐）

使用 `build.sh` 脚本快速构建：

```bash
# Debug 构建（开发/调试）
./build.sh Debug

# Release 构建（测试/部署）
./build.sh Release

# 清理并重新构建
./build.sh Debug clean
./build.sh Release clean
```

### 手动构建

#### 1. 创建构建目录

```bash
mkdir -p build && cd build
```

#### 2. 配置 CMake

```bash
# Debug 模式（开发/调试）
cmake -DCMAKE_BUILD_TYPE=Debug ..

# Release 模式（测试/部署）
cmake -DCMAKE_BUILD_TYPE=Release ..

# 或启用所有调试日志（即使在 Release 模式）
cmake -DCMAKE_BUILD_TYPE=Release -DENABLE_DEBUG_LOGS=ON ..
```

#### 3. 编译

```bash
# 使用多线程编译加速
make -j$(nproc)  # Linux
make -j$(sysctl -n hw.ncpu)  # macOS
```

#### 4. 编译成果

编译成功后，可执行文件位于：
```
test/smtpsServer
```

### 构建模式对比

| 特性 | Debug 模式 | Release 模式 |
|------|-----------|------------|
| **优化级别** | `-O0`（无优化） | `-O3`（高优化） |
| **本地优化** | ❌ | ✅ `-march=native` |
| **调试符号** | ✅ `-g` | ❌ |
| **日志级别** | DEBUG（全部） | INFO（仅 INFO 和 WARNING） |
| **编译时间** | 快 | 较慢 |
| **运行时速度** | 慢 | 快 |
| **二进制大小** | 大 | 小 |

**详细构建指南**: `BUILD_GUIDE.md`

## ⚙️ 配置文件

### 1. 数据库配置 (`config/db_config.json`)

数据库配置文件，需要根据你的实际 MySQL 服务器信息修改：

```json
{
  "achieve": "mysql",
  "host": "localhost",
  "user": "mail_test",
  "password": "your_password_here",
  "database": "mail",
  "initialize_script": "sql/create_tables.sql",
  "port": 3306,
  "initial_pool_size": 32,
  "max_pool_size": 128,
  "connection_timeout": 5,
  "idle_timeout": 300
}
```

**配置说明**:
- `host`: MySQL 服务器地址
- `user`: 数据库用户名
- `password`: 数据库密码
- `database`: 数据库名称
- `initialize_script`: 初始化 SQL 脚本路径
- `initial_pool_size`: 初始连接池大小
- `max_pool_size`: 最大连接池大小
- `connection_timeout`: 连接超时（秒）
- `idle_timeout`: 空闲连接超时（秒）

**实测配置（MacBook M2 Pro）**:
- `initial_pool_size: 32`
- `max_pool_size: 128`
- `connection_timeout: 5`
- `idle_timeout: 300`

### 2. 创建数据库

```bash
# 登录 MySQL
mysql -u root -p

# 创建数据库和用户
CREATE DATABASE mail CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
CREATE USER 'mail_test'@'localhost' IDENTIFIED BY 'your_password_here';
GRANT ALL PRIVILEGES ON mail.* TO 'mail_test'@'localhost';
FLUSH PRIVILEGES;

# 导入表结构
USE mail;
SOURCE sql/create_tables.sql;
```

### 3. SMTPS 服务器配置 (`config/smtpsConfig.json`)

服务器配置文件：

```json
{
  "address": "0.0.0.0",
  "port": 465,
  "use_ssl": true,
  "enable_ssl": true,
  "enable_tcp": true,
  "ssl_port": 465,
  "tcp_port": 25,
  "certFile": "crt/server.crt",
  "keyFile": "crt/server.key",
  "dhFile": "",
  "maxMessageSize": 1048576,
  "maxConnections": 1000,
  "io_thread_count": 24,
  "worker_thread_count": 12,
  "use_database": true,
  "db_config_file": "db_config.json",
  "connection_timeout": 300,
  "read_timeout": 60,
  "write_timeout": 60,
  "require_auth": false,
  "max_auth_attempts": 3,
  "log_level": "info",
  "log_file": "../logs/server.log",
  "mail_storage_path": "../mail/",
  "attachment_storage_path": "../attachments/"
}
```

**配置说明**:
- `address`: 监听地址（0.0.0.0 表示所有接口）
- `ssl_port`: SSL 监听端口（默认 465）
- `tcp_port`: TCP 监听端口（默认 25）
- `certFile`: SSL 证书文件路径
- `keyFile`: SSL 私钥文件路径
- `io_thread_count`: IO 线程数（建议：CPU 核数的 50-75%）
- `worker_thread_count`: 工作线程数（建议：CPU 核数的 75-150%）
- `log_file`: 日志文件路径
- `mail_storage_path`: 邮件存储路径
- `attachment_storage_path`: 附件存储路径

**实测配置（MacBook M2 Pro）**:
- `io_thread_count: 24`
- `worker_thread_count: 12`
- 性能：~180 msg/s

### 4. SSL 证书配置

如果使用 SSL/TLS，需要准备 SSL 证书：

#### 选项 1: 自签名证书（测试环境）

```bash
# 创建证书目录
mkdir -p config/crt

# 生成自签名证书
openssl req -x509 -newkey rsa:4096 -keyout config/crt/server.key \
    -out config/crt/server.crt -days 365 -nodes \
    -subj "/CN=localhost"
```

#### 选项 2: Let's Encrypt 免费证书（生产环境）

```bash
# 安装 certbot
sudo apt-get install certbot  # Ubuntu
brew install certbot           # macOS

# 为域名申请证书
sudo certbot certonly --standalone -d yourdomain.com

# 复制证书到项目目录
sudo cp /etc/letsencrypt/live/yourdomain.com/fullchain.pem config/crt/server.crt
sudo cp /etc/letsencrypt/live/yourdomain.com/privkey.pem config/crt/server.key

# 设置权限
sudo chmod 644 config/crt/server.crt
sudo chmod 600 config/crt/server.key
```

## 🎯 启动服务器

### 1. 创建必要的目录

```bash
cd /path/to/mail-system
mkdir -p logs mail attachments
```

### 2. 运行服务器

```bash
# 进入 build 目录
cd /path/to/mail-system

# 启动服务器
./test/smtpsServer
```

### 3. 查看日志

```bash
# 实时查看日志
tail -f ../logs/server.log

# 只看错误日志
tail -f ../logs/server.log | grep -E "(error|critical)"

# 只看网络连接日志
tail -f ../logs/server.log | grep NETWORK
```

## 📬 测试邮件发送

### 使用 telnet 测试

```bash
# 测试 SMTP (端口 25)
telnet localhost 25

# 测试 SMTPS (端口 465)
openssl s_client -connect localhost:465 -quiet
```

### SMTP 命令示例

```
EHLO yourdomain.com
MAIL FROM: <sender@example.com>
RCPT TO: <recipient@example.com>
DATA
Subject: Test Email
From: sender@example.com
To: recipient@example.com

This is a test email.
.
QUIT
```

### 使用 Python 测试脚本

```python
import smtplib
from email.mime.text import MIMEText

# SMTPS (端口 465)
server = smtplib.SMTP_SSL('localhost', 465)
server.login('username', 'password')  # 如果需要认证
msg = MIMEText('Test email content')
msg['Subject'] = 'Test Email'
msg['From'] = 'sender@example.com'
msg['To'] = 'recipient@example.com'
server.send_message(msg)
server.quit()
```

### 使用 swaks 测试附件传输

[swaks](http://www.jetmore.org/john/code/swaks/) 是一个功能强大的 SMTP 测试工具，支持测试附件传输功能。

#### 安装 swaks

```bash
# macOS
brew install swaks

# Ubuntu/Debian
sudo apt-get install swaks

# CentOS/RHEL
sudo yum install swaks
```

#### 发送带附件的邮件

```bash
# 发送带文本附件的邮件
swaks --to recipient@example.com \
      --from sender@example.com \
      --server localhost:465 \
      --auth \
      --auth-user username \
      --auth-password password \
      --tls \
      --body "Email body text here" \
      --attach /path/to/file.txt

# 发送带多个附件的邮件
swaks --to recipient@example.com \
      --from sender@example.com \
      --server localhost:465 \
      --auth \
      --auth-user username \
      --auth-password password \
      --tls \
      --body "Multiple attachments test" \
      --attach /path/to/file1.txt \
      --attach /path/to/file2.pdf \
      --attach /path/to/image.jpg

# 测试大文件附件
swaks --to recipient@example.com \
      --from sender@example.com \
      --server localhost:465 \
      --auth \
      --auth-user username \
      --auth-password password \
      --tls \
      --body "Large file attachment test" \
      --attach /path/to/largefile.zip

# 不使用认证发送邮件（如果服务器不需要认证）
swaks --to recipient@example.com \
      --from sender@example.com \
      --server localhost:465 \
      --tls \
      --body "Test email without auth" \
      --attach /path/to/file.txt

# 使用明文 SMTP (端口 25)
swaks --to recipient@example.com \
      --from sender@example.com \
      --server localhost:25 \
      --body "Test email via port 25" \
      --attach /path/to/file.txt
```

#### swaks 参数说明

- `--to`: 收件人邮箱地址
- `--from`: 发件人邮箱地址
- `--server`: 邮件服务器地址和端口
- `--auth`: 启用 SMTP 认证
- `--auth-user`: 认证用户名
- `--auth-password`: 认证密码
- `--tls`: 使用 TLS 加密（SMTPS）
- `--body`: 邮件正文内容
- `--attach`: 指定附件文件路径（可多次使用）
- `--header`: 添加自定义邮件头
- `--data`: 从文件读取完整的邮件内容

#### 高级用法示例

```bash
# 自定义邮件头
swaks --to recipient@example.com \
      --from sender@example.com \
      --server localhost:465 \
      --tls \
      --header "Subject: Test Email with Custom Headers" \
      --header "X-Custom-Header: Custom Value" \
      --body "Test email body" \
      --attach /path/to/file.txt

# 从文件读取邮件正文
swaks --to recipient@example.com \
      --from sender@example.com \
      --server localhost:465 \
      --tls \
      --body @/path/to/body.txt \
      --attach /path/to/file.txt

# 显示详细调试信息
swaks --to recipient@example.com \
      --from sender@example.com \
      --server localhost:465 \
      --tls \
      --body "Debug test" \
      --attach @/path/to/file.txt \
      --verbose \
      --show-mail-path
```

## 🌐 域名部署

### 部署到域名

#### 1. DNS 配置

在你的域名管理后台添加 DNS 记录：

```
类型    主机记录      记录值              TTL
A       mail          你的服务器公网IP     600
MX      @            mail.yourdomain.com  600
TXT     @            "v=spf1 mx ~all"     600
```

#### 2. 防火墙配置

开放必要的端口：

```bash
# Ubuntu/Debian (ufw)
sudo ufw allow 25/tcp    # SMTP
sudo ufw allow 465/tcp   # SMTPS
sudo ufw allow 587/tcp   # Submission (STARTTLS)
sudo ufw reload

# CentOS/RHEL (firewalld)
sudo firewall-cmd --permanent --add-port=25/tcp
sudo firewall-cmd --permanent --add-port=465/tcp
sudo firewall-cmd --permanent --add-port=587/tcp
sudo firewall-cmd --reload
```

#### 3. 配置服务器监听公网 IP

修改 `config/smtpsConfig.json`:

```json
{
  "address": "0.0.0.0",
  "ssl_port": 465,
  "tcp_port": 25
}
```

#### 4. 使用域名申请 SSL 证书

```bash
# 为域名申请 Let's Encrypt 证书
sudo certbot certonly --standalone -d your_domain.com

# 复制证书到项目目录
sudo cp /etc/letsencrypt/live/your_domain.com/fullchain.pem config/crt/server.crt
sudo cp /etc/letsencrypt/live/your_domain.com/privkey.pem config/crt/server.key
# 设置权限
sudo chmod 644 config/crt/server.crt
sudo chmod 600 config/crt/server.key
```

#### 5. 验证部署

```bash
# 测试域名解析
nslookup your_domain.com

# 测试端口连通性
telnet your_domain.com 465
openssl s_client -connect your_domain.com:465

# 测试邮件发送（使用 QQ 邮箱）
python3 << 'EOF'
import smtplib
from email.mime.text import MIMEText

# 通过你的服务器发送邮件
server = smtplib.SMTP_SSL('your_domain.com', 465)
server.set_debuglevel(1)  # 显示调试信息

msg = MIMEText('Test email from SMTPS server')
msg['Subject'] = 'Test Email'
msg['From'] = 'your-email@your-domain.com'
msg['To'] = 'your-qq@qq.com'

server.send_message(msg)
server.quit()
print('Email sent successfully!')
EOF
```

## 🔧 高级配置

### 启用特定模块的调试日志

编辑 `include/mail_system/back/common/logger.h`，修改对应的宏：

```cpp
// 启用数据库调试日志
#define ENABLE_DATABASE_DEBUG_LOG 1
#define ENABLE_DATABASE_QUERY_DEBUG_LOG 1

// 启用 SMTP 详细日志
#define ENABLE_SMTP_DETAIL_DEBUG_LOG 1
```

重新编译：

```bash
cd build
cmake ..
make -j$(nproc)
```

### 调整日志级别

在 `test/smtps_test.cpp` 中修改初始化参数：

```cpp
Logger::get_instance().init(
    "logs/mail_system.log",     // 日志文件路径
    1024 * 1024 * 10,           // 单文件大小 10MB
    5,                           // 保留 5 个文件
    spdlog::level::debug         // 日志级别
);
```

日志级别：
- `trace` - 最详细
- `debug` - 调试信息
- `info` - 一般信息（生产默认）
- `warn` - 警告
- `error` - 错误
- `critical` - 严重错误

## 📂 项目结构

```
mail-system/v7/
├── CMakeLists.txt              # CMake 构建配置
├── build.sh                   # 快速构建脚本
├── BUILD_GUIDE.md             # 详细的构建指南
├── README.md                  # 本文件
├── config/                     # 配置文件目录
│   ├── smtpsConfig.json       # SMTPS 服务器配置
│   ├── db_config.json         # 数据库配置
│   └── crt/                   # SSL 证书目录
│       ├── server.crt        # SSL 证书
│       └── server.key        # SSL 私钥
├── include/mail_system/back/   # 头文件目录
│   ├── common/                # 通用组件（日志）
│   ├── db/                    # 数据库层
│   ├── entities/              # 实体类
│   ├── mailServer/            # 邮件服务器
│   │   ├── connection/       # 连接层
│   │   ├── session/          # 会话层
│   │   └── fsm/              # 状态机
│   ├── persist_storage/     # 持久化存储
│   ├── thread_pool/          # 线程池
│   └── algorithm/            # 算法工具
├── src/mail_system/back/       # 源文件目录
├── sql/                       # SQL 脚本
│   └── create_tables.sql     # 数据库表结构
├── test/                      # 测试和入口
│   └── smtps_test.cpp        # 主程序
├── docs/                      # 文档目录
│   ├── ARCHITECTURE.md       # 架构设计文档
│   ├── logging-guide.md      # 日志系统说明
│   └── ...
├── logs/                      # 日志文件目录（自动生成）
├── mail/                      # 邮件存储目录（自动生成）
├── attachments/               # 附件存储目录（自动生成）
├── OuterLib/                 # 外部库（nlohmann/json）
└── build/                    # 构建目录（自动生成）
```

## 📚 文档

详细文档请参考：

- [架构设计文档](docs/ARCHITECTURE.md) - 完整的系统架构和设计思想
- [构建指南](BUILD_GUIDE.md) - 详细的构建配置说明
- [日志系统说明](docs/logging-guide.md) - 日志配置和使用指南
- [日志配置完成报告](docs/logging-completion.md) - 日志系统集成总结
- [数据库问题分析](docs/prepared-statement-connection-pool-issue.md) - Prepared Statement 兼容性问题
- [快速配置指南](docs/quick-log-config.md) - 快速配置参考
- [域名部署指南](docs/domain-deployment-guide.md) - 域名和 SSL 证书配置

## ⚡ 性能指标与配置建议

### 性能基准测试

以下是基于实际测试结果的性能数据：

| 硬件配置 | 并发连接数 | 邮件/秒 | 邮件/小时 | CPU 核数 | 内存 | 磁盘 IOPS |
|---------|-----------|---------|-----------|----------|------|-----------|
| **实际测试环境** |  |  |  |  |  |  |
| MacBook Pro M2 | 32 | **~180** | **~648,000** | 12核 | 16GB | SSD (NVMe) |

**测试配置**：
- io_thread_count: 24
- worker_thread_count: 12
- 数据库连接池: initial=32, max=128
- 邮件大小: ~10KB
- 测试工具: cl.py (Python SMTP 客户端)

**说明**：
- 300 封和 600 封邮件测试均保持 ~180 msg/s 的稳定速度
- 性能瓶颈可能受限于磁盘 I/O 或数据库写入速度
- M2 Pro 性能核心 8 核 + 能效核心 4 核，实际可利用约 8-10 核

### 影响性能的关键因素

#### 1. CPU 配置

| CPU 核数 | 推荐配置 | 说明 |
|---------|---------|------|
| 4 核 | `io_thread_count: 2`<br>`worker_thread_count: 2` | 基础配置，适合小型部署 |
| 8 核 | `io_thread_count: 3-4`<br>`worker_thread_count: 4-6` | 推荐配置，平衡 IO 和计算 |
| 12 核 (M2 Pro 实测) | `io_thread_count: 24`<br>`worker_thread_count: 12` | 当前实测配置 |
| 16 核 | `io_thread_count: 4-6`<br>`worker_thread_count: 8-12` | 高性能配置，充分利用多核 |
| 32 核+ | `io_thread_count: 6-8`<br>`worker_thread_count: 16-24` | 企业级配置 |

**说明**：
- `io_thread_count`: 网络 IO 线程数，通常为 CPU 核数的 50-75%
- `worker_thread_count`: 工作线程数（持久化、附件处理等），通常为 CPU 核数的 75-150%
- M2 Pro 实测使用 24 IO 线程 + 12 工作线程，性能稳定在 ~180 msg/s

#### 2. 数据库配置

| 并发量 | 数据库配置 | 说明 |
|--------|-----------|------|
| 低并发 (<100) | `initial_pool_size: 5`<br>`max_pool_size: 20` | 适合小型测试环境 |
| 中并发 (100-500) | `initial_pool_size: 10`<br>`max_pool_size: 50` | 推荐配置 |
| 高并发 (500+) | `initial_pool_size: 20`<br>`max_pool_size: 100`<br>`connection_timeout: 3` | 需要配合 MySQL 优化 |

**实测配置（MacBook M2 Pro）**：
- `initial_pool_size: 32`
- `max_pool_size: 128`
- `connection_timeout: 5`
- `idle_timeout: 300`

**MySQL 优化建议**：

```sql
-- my.cnf 配置示例（针对高并发场景）
[mysqld]
# 连接数配置
max_connections = 500
max_connect_errors = 1000

# InnoDB 配置
innodb_buffer_pool_size = 4G      # 设为物理内存的 50-70%
innodb_log_file_size = 512M
innodb_flush_log_at_trx_commit = 2  # 提高写入性能
innodb_flush_method = O_DIRECT

# 查询缓存
query_cache_size = 0               # 禁用查询缓存（MySQL 8.0+ 已移除）

# 临时表
tmp_table_size = 256M
max_heap_table_size = 256M

# 线程配置
thread_cache_size = 100
```

#### 3. 磁盘 I/O 配置

| 磁盘类型 | 预期性能 | 适用场景 |
|---------|---------|---------|
| HDD (7200 RPM) | ~100 IOPS | 仅适合测试，生产环境不推荐 |
| SATA SSD | ~50,000 IOPS | 小型部署 |
| NVMe SSD | ~200,000+ IOPS | 推荐用于生产环境 |
| NVMe RAID 0 | ~400,000+ IOPS | 高性能场景 |
| 企业级 SSD | ~500,000+ IOPS | 大规模部署 |

**磁盘优化建议**：
- 邮件存储和附件存储使用独立磁盘（或分区）
- 使用 XFS 或 ext4 文件系统
- 启用 `noatime` 挂载选项减少磁盘写入

#### 4. 网络配置

| 网络带宽 | 支持并发 | 预期吞吐 |
|---------|---------|---------|
| 100 Mbps | ~50 并发 | ~10 MB/s |
| 1 Gbps | ~500 并发 | ~100 MB/s |
| 10 Gbps | ~5000 并发 | ~1 GB/s |

**系统优化建议**：

```bash
# /etc/sysctl.conf 配置
# 增加网络连接队列长度
net.core.somaxconn = 65535
net.core.netdev_max_backlog = 5000

# 增加文件描述符限制
fs.file-max = 1000000

# TCP 优化
net.ipv4.tcp_max_syn_backlog = 8192
net.ipv4.tcp_tw_reuse = 1
net.ipv4.ip_local_port_range = 10000 65535

# 应用配置（在启动脚本中）
ulimit -n 100000  # 增加文件描述符限制
```

### 性能调优清单

#### 基础优化（必做）

- [ ] 根据硬件配置调整线程数（`io_thread_count`, `worker_thread_count`）
- [ ] 配置合适的数据库连接池大小（`max_pool_size`）
- [ ] 使用 NVMe SSD 存储邮件和附件
- [ ] 增加系统文件描述符限制
- [ ] 配置合适的网络缓冲区大小

#### 进阶优化（推荐）

- [ ] MySQL `innodb_buffer_pool_size` 设为物理内存的 50-70%
- [ ] 使用 SSD 存储数据库文件
- [ ] 启用 `innodb_flush_log_at_trx_commit = 2` 提高写入性能
- [ ] 邮件存储和附件存储使用独立磁盘
- [ ] 配置 RAID 0 或 RAID 10 提升磁盘性能

#### 高级优化（大规模部署）

- [ ] 使用多个邮件服务器实例（负载均衡）
- [ ] 部署读写分离的 MySQL 主从架构
- [ ] 使用 Redis 缓存热点数据
- [ ] 配置 CDN 加速附件下载
- [ ] 实现邮件队列的分布式架构

### 性能测试方法

### 使用python脚本进行测试

uv run ./test/cl.py --messages 300

#### 使用 swaks 进行压力测试

```bash
# 单线程测试（基准测试）
for i in {1..1000}; do
    swaks --to test@example.com \
          --from sender@example.com \
          --server localhost:465 \
          --tls \
          --body "Test email $i" &
done

# 多线程并发测试（压力测试）
#!/bin/bash
CONCURRENT=100
TOTAL_EMAILS=10000

for ((i=0; i<TOTAL_EMAILS; i++)); do
    swaks --to test@example.com \
          --from sender@example.com \
          --server localhost:465 \
          --tls \
          --body "Stress test email $i" &

    # 控制并发数
    if ((i % CONCURRENT == 0)); then
        wait
    fi
done
wait
```

#### 监控关键指标

```bash
# 实时监控 CPU 使用率
top -p $(pgrep -d',' smtpsServer)

# 监控网络连接数
watch -n 1 'netstat -an | grep 465 | wc -l'

# 监控磁盘 I/O
iostat -x 1

# 监控数据库连接
watch -n 1 'mysql -u root -p -e "SHOW PROCESSLIST;" | wc -l'

# 查看日志统计
tail -f logs/mail_system.log | grep -E "Successfully processed|ERROR"
```

### 常见性能瓶颈

| 问题症状 | 可能原因 | 解决方案 |
|---------|---------|---------|
| 处理速度慢，CPU 使用率低 | 磁盘 I/O 瓶颈 | 升级到 NVMe SSD，优化数据库 |
| CPU 使用率 100% | 线程数配置不当 | 增加 `worker_thread_count` |
| 大量超时错误 | 数据库连接池耗尽 | 增加 `max_pool_size` |
| 内存占用过高 | 并发数过多 | 限制 `maxConnections` |
| 网络连接被拒绝 | 文件描述符限制 | 增加 `ulimit -n` 设置 |

### 极限性能场景配置示例

```json
{
  "address": "0.0.0.0",
  "port": 465,
  "use_ssl": true,
  "enable_ssl": true,
  "enable_tcp": true,
  "ssl_port": 465,
  "tcp_port": 25,
  "certFile": "crt/server.crt",
  "keyFile": "crt/server.key",
  "dhFile": "",
  "maxMessageSize": 52428800,
  "maxConnections": 2000,
  "io_thread_count": 8,
  "worker_thread_count": 24,
  "use_database": true,
  "db_config_file": "db_config.json",
  "connection_timeout": 300,
  "read_timeout": 60,
  "write_timeout": 60,
  "require_auth": false,
  "max_auth_attempts": 3,
  "log_level": "info",
  "log_file": "../logs/server.log",
  "mail_storage_path": "../mail/",
  "attachment_storage_path": "../attachments/"
}
```

```json
{
  "achieve": "mysql",
  "host": "localhost",
  "user": "mail_user",
  "password": "your_password",
  "database": "mail",
  "initialize_script": "sql/create_tables.sql",
  "port": 3306,
  "initial_pool_size": 20,
  "max_pool_size": 100,
  "connection_timeout": 3,
  "idle_timeout": 60
}
```

### 下一步改进方向

- 使用特化内存池分配会话所需内存，减少内存碎片
- 邮件解析使用simd加速

## 📚 文档

详细文档请参考：

- [架构设计文档](docs/ARCHITECTURE.md) - 完整的系统架构和设计思想
- [构建指南](BUILD_GUIDE.md) - 详细的构建配置说明
- [日志系统说明](docs/logging-guide.md) - 日志配置和使用指南
- [日志配置完成报告](docs/logging-completion.md) - 日志系统集成总结
- [数据库问题分析](docs/prepared-statement-connection-pool-issue.md) - Prepared Statement 兼容性问题
- [快速配置指南](docs/quick-log-config.md) - 快速配置参考
- [域名部署指南](docs/domain-deployment-guide.md) - 域名和 SSL 证书配置

## 🐛 常见问题

### 1. 编译错误：找不到 Boost

```bash
# 检查 Boost 安装
find /usr -name "libboost_system.so" 2>/dev/null

# 指定 Boost 路径
cmake -DBOOST_ROOT=/usr/local/boost ..
```

### 2. 编译错误：找不到 OpenSSL

```bash
# 指定 OpenSSL 路径
cmake -DOPENSSL_ROOT_DIR=/usr/local/ssl ..
```

### 3. 运行时错误：无法连接数据库

检查 `config/db_config.json` 中的数据库配置是否正确，确保 MySQL 服务正在运行。

### 4. SSL 握手失败

- 检查证书文件路径是否正确
- 检查证书文件权限（私钥应为 600）
- 确认域名解析到正确的 IP

### 5. 邮件无法发送

- 检查防火墙是否开放 25/465 端口
- 查看日志文件排查具体错误
- 使用 `telnet` 或 `openssl s_client` 测试连接

## 🤝 贡献

欢迎提交 Issue 和 Pull Request！

## 📄 许可证

本项目采用 MIT 许可证。

## 📧 联系方式

如有问题，请通过以下方式联系：

- 提交 Issue: [GitHub Issues](https://github.com/deepdiver-997/my-mail-system/issues)

---

**注意**: 本项目仅用于学习和研究目的，生产环境使用请务必配置 SSL/TLS、认证、反垃圾邮件等安全措施。

## 📄 Third-party Libraries and License Notes

### nlohmann/json

This project includes the single-header version of nlohmann/json (in `OuterLib/json`) for convenient JSON parsing. nlohmann/json is distributed under the MIT License. We include it here as a copy of the upstream single-header implementation for educational purposes.

Attribution:

- nlohmann/json — JSON for Modern C++ (single-header)
    - Repository: https://github.com/nlohmann/json
    - License: MIT License

If you redistribute this project or produce a binary including nlohmann/json, you must keep the MIT license text available (the upstream license permits redistribution with attribution). The included single-header comes with its MIT license; ensure you preserve that file's license header if you vendor/update it.

### Boost

This project links against Boost. If you redistribute binaries or vendor Boost headers, include the Boost Software License text. A copy is provided in `COPYING_BOOST.txt` in the project root.

### OpenSSL

This project links to OpenSSL. OpenSSL's licensing depends on the version:

- OpenSSL 1.1.x and earlier: OpenSSL License + SSLeay License (see upstream for exact text).
- OpenSSL 3.x: Apache License 2.0.

If you redistribute OpenSSL with your binaries, ensure you comply with the applicable OpenSSL licensing terms and include its license files and attribution. For redistribution, include the OpenSSL license text that came with your OpenSSL installation, or reference the upstream license page:

https://www.openssl.org/source/license.html

### Notes about the Boost license

This project links against Boost. If you redistribute binaries or vendor Boost headers, include the Boost Software License text. A copy is provided in `COPYING_BOOST.txt` in the project root.
