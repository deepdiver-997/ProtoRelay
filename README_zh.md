# Mail System V6 - SMTPS 邮件服务器（中文版）

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

### 1. 克隆或进入项目目录

```bash
cd /path/to/mail-system
```

### 2. 创建构建目录

```bash
mkdir build && cd build
```

### 3. 配置 CMake

```bash
# 默认配置（生产环境）
cmake ..

# 或启用所有调试日志（开发环境）
cmake -DENABLE_DEBUG_LOGS=ON ..
```

### 4. 编译

```bash
# 使用多线程编译加速
make -j$(nproc)  # Linux
make -j$(sysctl -n hw.ncpu)  # macOS
```

### 5. 编译成果

编译成功后，可执行文件位于：
```
build/test/smtpsServer
```

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
  "initial_pool_size": 5,
  "max_pool_size": 10,
  "connection_timeout": 5,
  "idle_timeout": 60
}
```

**配置说明**:
- `host`: MySQL 服务器地址
- `user`: 数据库用户名
- `password`: 数据库密码
- `database`: 数据库名称
- `initialize_script`: 初始化 SQL 脚本路径

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
  "io_thread_count": 4,
  "worker_thread_count": 4,
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
- `log_file`: 日志文件路径
- `mail_storage_path`: 邮件存储路径
- `attachment_storage_path`: 附件存储路径

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
cd build

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
      --attach /path/to/file.txt \
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
mail-system/
├── CMakeLists.txt              # CMake 构建配置
├── README.md                   # 英文文档
├── README_zh.md                # 中文文档
├── .gitignore                  # Git 忽略文件
├── COPYING_BOOST.txt           # Boost 许可证
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
│   │   ├── fsm/              # 状态机
│   │   └── ...
│   └── thread_pool/          # 线程池
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
└── build/                    # 构建目录（自动生成）
```

## 📚 文档

详细文档请参考：

- [架构设计文档](docs/ARCHITECTURE.md) - 完整的系统架构和设计思想
- [日志系统说明](docs/logging-guide.md) - 日志配置和使用指南
- [日志配置完成报告](docs/logging-completion.md) - 日志系统集成总结
- [数据库问题分析](docs/prepared-statement-connection-pool-issue.md) - Prepared Statement 兼容性问题
- [快速配置指南](docs/quick-log-config.md) - 快速配置参考

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

## 📄 第三方库与许可说明

### nlohmann/json

本项目包含 nlohmann/json 的单文件版本（位于 `OuterLib/json`）用于 JSON 解析。nlohmann/json 使用 MIT 许可发布，我们以 vendor 形式包含其单文件实现以便学习和实验使用。

- **仓库**: https://github.com/nlohmann/json
- **许可**: MIT License
- **说明**: 在二进制分发或再发布本项目时，请务必保留 nlohmann/json 的 MIT 许可文本（单文件头部自带许可信息），以符合集成许可要求。

### Boost

本项目链接并使用 Boost 库。如需再分发二进制或 vendor Boost 头文件，请随包附上 Boost Software License 文本。Boost 许可证文件位于项目根目录 `COPYING_BOOST.txt`。

### OpenSSL

本项目链接到 OpenSSL。OpenSSL 拥有自己的许可与归属说明：

- OpenSSL 1.1.x 及更早版本：OpenSSL License + SSLeay License
- OpenSSL 3.x：Apache License 2.0

如果你随二进制再分发 OpenSSL，请遵守 OpenSSL 的许可要求并随包附上相应许可文本。详情见：https://www.openssl.org/source/license.html

### 关于 Boost 许可

本项目链接并使用 Boost 库。如果需要再分发二进制或分发 Boost 头文件，请包含 Boost Software License 文本。项目根目录提供了 `COPYING_BOOST.txt` 文件供参考。
