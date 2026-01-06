# 域名部署指南 - www.hgmail.xin

本文档详细介绍如何将 SMTPS 邮件服务器部署到域名 `www.hgmail.xin`，并配置 SSL/TLS 证书。

## 目录

1. [前置准备](#前置准备)
2. [DNS 配置](#dns-配置)
3. [服务器环境配置](#服务器环境配置)
4. [SSL 证书申请](#ssl-证书申请)
5. [服务器配置](#服务器配置)
6. [防火墙配置](#防火墙配置)
7. [部署验证](#部署验证)
8. [使用 QQ 邮箱测试](#使用-qq-邮箱测试)
9. [邮件服务器优化](#邮件服务器优化)
10. [常见问题](#常见问题)

---

## 前置准备

### 1. 硬件要求

- **CPU**: 2 核心以上
- **内存**: 2GB 以上
- **存储**: 20GB 以上
- **网络**: 稳定的公网 IP

### 2. 软件要求

- **操作系统**: Linux (Ubuntu 20.04+, CentOS 7+) 或 macOS
- **MySQL**: 5.7+ 或 8.0+
- **CMake**: 3.10+
- **编译器**: GCC 9+ / Clang 10+

### 3. 域名要求

- 域名已解析到服务器公网 IP
- 可以控制域名 DNS 记录

---

## DNS 配置

### 1. 基础 DNS 记录

在你的域名管理后台（如阿里云、腾讯云、Cloudflare 等）添加以下记录：

```
类型    主机记录      记录值                   TTL
A       mail          <你的服务器公网IP>        600
A       www           <你的服务器公网IP>        600
A       @             <你的服务器公网IP>        600
MX      @             mail.hgmail.xin           600 (优先级 10)
TXT     @             "v=spf1 mx ~all"         600
```

### 2. 验证 DNS 配置

```bash
# 检查 A 记录
nslookup mail.hgmail.xin
nslookup www.hgmail.xin

# 检查 MX 记录
nslookup -type=mx hgmail.xin

# 检查 SPF 记录
dig txt hgmail.xin
```

预期输出示例：
```
$ nslookup mail.hgmail.xin
Server:  8.8.8.8
Address: 8.8.8.8#53

Non-authoritative answer:
Name:    mail.hgmail.xin
Address: 123.45.67.89  # 你的服务器公网 IP
```

---

## 服务器环境配置

### 1. 安装依赖（Ubuntu/Debian）

```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    git \
    libboost-all-dev \
    libssl-dev \
    libmysqlclient-dev \
    libspdlog-dev \
    mysql-server \
    certbot
```

### 2. 安装依赖（CentOS/RHEL）

```bash
sudo yum groupinstall -y "Development Tools"
sudo yum install -y \
    cmake \
    git \
    boost-devel \
    openssl-devel \
    mysql-devel \
    mysql-server \
    certbot
```

### 3. 配置 MySQL

```bash
# 启动 MySQL 服务
sudo systemctl start mysql
sudo systemctl enable mysql

# 登录 MySQL
sudo mysql -u root -p

# 在 MySQL 中执行以下命令
CREATE DATABASE mail CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
CREATE USER 'mail_test'@'%' IDENTIFIED BY 'your_strong_password';
GRANT ALL PRIVILEGES ON mail.* TO 'mail_test'@'%';
FLUSH PRIVILEGES;
```

**重要**: 将 `your_strong_password` 替换为强密码，建议包含大小写字母、数字和特殊字符。

---

## SSL 证书申请

### 方案一：使用 Let's Encrypt（推荐）

#### 1. 申请证书

```bash
# 为域名申请证书
sudo certbot certonly --standalone -d mail.hgmail.xin -d www.hgmail.xin

# 按提示输入邮箱（用于证书续期提醒）
# 同意服务条款
# 选择是否分享邮箱
```

#### 2. 证书位置

证书申请成功后，证书文件位于：
```
/etc/letsencrypt/live/mail.hgmail.xin/fullchain.pem  # 证书链
/etc/letsencrypt/live/mail.hgmail.xin/privkey.pem    # 私钥
/etc/letsencrypt/live/mail.hgmail.xin/cert.pem        # 证书
```

#### 3. 复制证书到项目目录

```bash
# 进入项目目录
cd /path/to/mail-system/v6

# 创建证书目录
mkdir -p config/crt

# 复制证书
sudo cp /etc/letsencrypt/live/mail.hgmail.xin/fullchain.pem config/crt/server.crt
sudo cp /etc/letsencrypt/live/mail.hgmail.xin/privkey.pem config/crt/server.key

# 设置权限
sudo chmod 644 config/crt/server.crt
sudo chmod 600 config/crt/server.key

# 确保所有者是运行用户
sudo chown $USER:$USER config/crt/server.crt
sudo chown $USER:$USER config/crt/server.key
```

#### 4. 配置自动续期

```bash
# 测试续期
sudo certbot renew --dry-run

# 添加续期脚本
sudo tee /etc/cron.weekly/certbot-renew.sh > /dev/null <<'EOF'
#!/bin/bash
certbot renew --quiet
cd /path/to/mail-system/v6
sudo cp /etc/letsencrypt/live/mail.hgmail.xin/fullchain.pem config/crt/server.crt
sudo cp /etc/letsencrypt/live/mail.hgmail.xin/privkey.pem config/crt/server.key
sudo chmod 644 config/crt/server.crt
sudo chmod 600 config/crt/server.key
sudo systemctl restart smtps-server  # 如果你配置了服务
EOF

# 设置可执行权限
sudo chmod +x /etc/cron.weekly/certbot-renew.sh
```

### 方案二：使用云服务商提供的免费证书

#### 阿里云

1. 在阿里云控制台申请免费 SSL 证书
2. 下载证书（Nginx 格式）
3. 解压后包含 `.pem` 和 `.key` 文件
4. 复制到 `config/crt/` 目录

#### 腾讯云

1. 在腾讯云控制台申请免费 SSL 证书
2. 下载证书（Nginx 格式）
3. 复制到 `config/crt/` 目录

---

## 服务器配置

### 1. 数据库配置

创建 `config/db_config.json`:

```json
{
  "achieve": "mysql",
  "host": "localhost",
  "user": "mail_test",
  "password": "your_strong_password",
  "database": "mail",
  "initialize_script": "sql/create_tables.sql",
  "port": 3306,
  "initial_pool_size": 5,
  "max_pool_size": 10,
  "connection_timeout": 5,
  "idle_timeout": 60
}
```

**重要**:
- 将 `your_strong_password` 替换为实际密码
- 如果 MySQL 在远程服务器，修改 `host` 为服务器地址

### 2. 导入数据库表结构

```bash
mysql -u mail_test -p mail < sql/create_tables.sql
```

### 3. SMTPS 服务器配置

修改 `config/smtpsConfig.json`:

```json
{
  "address": "0.0.0.0",
  "port": 465,
  "use_ssl": true,
  "enable_ssl": true,
  "enable_tcp": true,
  "ssl_port": 465,
  "tcp_port": 25,
  "certFile": "config/crt/server.crt",
  "keyFile": "config/crt/server.key",
  "dhFile": "",
  "maxMessageSize": 10485760,
  "maxConnections": 1000,
  "io_thread_count": 4,
  "worker_thread_count": 4,
  "use_database": true,
  "db_config_file": "config/db_config.json",
  "connection_timeout": 300,
  "read_timeout": 60,
  "write_timeout": 60,
  "require_auth": false,
  "max_auth_attempts": 3,
  "log_level": "info",
  "log_file": "logs/server.log",
  "mail_storage_path": "mail/",
  "attachment_storage_path": "attachments/"
}
```

**重要**:
- 确认证书路径正确
- 根据服务器配置调整线程数
- 生产环境建议设置 `"require_auth": true`

### 4. 创建必要的目录

```bash
cd /path/to/mail-system/v6
mkdir -p logs mail attachments
```

### 5. 编译项目

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

---

## 防火墙配置

### Ubuntu/Debian (UFW)

```bash
# 允许 SSH
sudo ufw allow 22/tcp

# 允许 SMTP 端口
sudo ufw allow 25/tcp    # SMTP
sudo ufw allow 465/tcp   # SMTPS
sudo ufw allow 587/tcp   # Submission (STARTTLS)

# 启用防火墙
sudo ufw enable

# 查看状态
sudo ufw status
```

### CentOS/RHEL (Firewalld)

```bash
# 启动防火墙
sudo systemctl start firewalld
sudo systemctl enable firewalld

# 开放端口
sudo firewall-cmd --permanent --add-port=25/tcp
sudo firewall-cmd --permanent --add-port=465/tcp
sudo firewall-cmd --permanent --add-port=587/tcp

# 重载防火墙
sudo firewall-cmd --reload

# 查看状态
sudo firewall-cmd --list-all
```

### 云服务商安全组

如果你使用阿里云、腾讯云等云服务器，还需要在云控制台配置安全组规则：

```
协议    端口范围      授权策略
TCP     25           允许 0.0.0.0/0
TCP     465          允许 0.0.0.0/0
TCP     587          允许 0.0.0.0/0
TCP     22           允许你的IP
```

---

## 部署验证

### 1. 启动服务器

```bash
cd /path/to/mail-system/v6/build
./test/smtpsServer
```

预期输出：
```
[2026-01-06 10:00:00.123] [info] [12345] [SERVER] Logger initialized
[2026-01-06 10:00:00.124] [info] [12345] [SERVER] Server initialized with SSL: enabled
[2026-01-06 10:00:00.125] [info] [12345] [SERVER] MySQL pool initialized
[2026-01-06 10:00:00.126] [info] [12345] [SERVER] SSL acceptor started on port 465
[2026-01-06 10:00:00.127] [info] [12345] [SERVER] TCP acceptor started on port 25
[2026-01-06 10:00:00.128] [info] [12345] [SERVER] Server is running...
```

### 2. 测试端口连通性

```bash
# 测试 SMTPS (465 端口)
openssl s_client -connect mail.hgmail.xin:465 -quiet

# 测试 SMTP (25 端口)
telnet mail.hgmail.xin 25
```

预期输出（SMTPS）:
```
CONNECTED(00000003)
220 SMTPS Server Ready
```

### 3. 检查 SSL 证书

```bash
openssl s_client -connect mail.hgmail.xin:465 -showcerts </dev/null
```

检查证书信息：
- 证书是否有效
- 证书主题是否为 `mail.hgmail.xin`
- 证书是否过期

---

## 使用 QQ 邮箱测试

### 1. Python 测试脚本

创建测试脚本 `test_email.py`:

```python
#!/usr/bin/env python3
import smtplib
from email.mime.text import MIMEText
from email.mime.multipart import MIMEMultipart
from email.header import Header
import ssl

# 配置
SMTP_SERVER = 'mail.hgmail.xin'
SMTP_PORT = 465
SENDER_EMAIL = 'test@hgmail.xin'
RECEIVER_EMAIL = 'your-qq@qq.com'  # 替换为你的 QQ 邮箱

# 创建邮件
msg = MIMEMultipart()
msg['From'] = Header(SENDER_EMAIL)
msg['To'] = Header(RECEIVER_EMAIL)
msg['Subject'] = Header('测试邮件 - SMTPS 服务器', 'utf-8')

# 邮件正文
body = '''
您好！

这是一封通过 SMTPS 服务器发送的测试邮件。

服务器信息：
- 服务器地址：mail.hgmail.xin
- 端口：465 (SMTPS)
- 发送时间：{time}

祝好！
'''.format(time=__import__('datetime').datetime.now().strftime('%Y-%m-%d %H:%M:%S'))

msg.attach(MIMEText(body, 'plain', 'utf-8'))

# 发送邮件
try:
    # 创建 SSL 上下文
    context = ssl.create_default_context()

    # 连接到 SMTPS 服务器
    print(f'连接到 {SMTP_SERVER}:{SMTP_PORT}...')
    server = smtplib.SMTP_SSL(SMTP_SERVER, SMTP_PORT, context=context)
    server.set_debuglevel(1)  # 显示调试信息

    # 发送邮件
    print('发送邮件...')
    server.sendmail(SENDER_EMAIL, RECEIVER_EMAIL, msg.as_string())

    # 关闭连接
    server.quit()
    print('\n✅ 邮件发送成功！')

except Exception as e:
    print(f'\n❌ 邮件发送失败: {e}')
```

### 2. 运行测试

```bash
python3 test_email.py
```

预期输出：
```
connect: to ('mail.hgmail.xin', 465)
connect: ('你的服务器IP', 465)
reply: b'220 SMTPS Server Ready'
reply: retcode (220); Msg: b'SMTPS Server Ready'
connect: mail.hgmail.xin
...
send: 'MAIL FROM:<test@hgmail.xin>\r\n'
reply: b'250 OK'
send: 'RCPT TO:<your-qq@qq.com>\r\n'
reply: b'250 OK'
send: 'DATA\r\n'
reply: b'354 End data with <CR><LF>.<CR><LF>'
data: (354, b'End data with <CR><LF>.<CR><LF>')
send: 'Subject: =?utf-8?q?=E6=B5=8B=E8=AF=95=E9=82=AE=E4=BB=B6_-SMTPS_=E6=9C=8D=E5=8A=A1=E5=99=A8?=\r\n...
reply: retcode (250); Msg: b'OK'
send: 'QUIT\r\n'
reply: b'221 Bye'

✅ 邮件发送成功！
```

### 3. 检查 QQ 邮箱

1. 登录 QQ 邮箱
2. 检查收件箱
3. 如果未收到，检查垃圾邮件箱

### 4. 使用 telnet 手动测试

```bash
# 连接到服务器
openssl s_client -connect mail.hgmail.xin:465 -quiet
```

SMTP 命令序列：
```
EHLO your-qq.com
MAIL FROM: <test@hgmail.xin>
RCPT TO: <your-qq@qq.com>
DATA
Subject: Test Email
From: test@hgmail.xin
To: your-qq@qq.com

This is a test email.
.
QUIT
```

---

## 邮件服务器优化

### 1. 配置系统服务（Systemd）

创建服务文件 `/etc/systemd/system/smtps-server.service`:

```ini
[Unit]
Description=SMTPS Mail Server
After=network.target mysql.service

[Service]
Type=simple
User=your-username
WorkingDirectory=/path/to/mail-system/v6/build
ExecStart=/path/to/mail-system/v6/build/test/smtpsServer
Restart=on-failure
RestartSec=10

[Install]
WantedBy=multi-user.target
```

启动服务：

```bash
# 重载 systemd
sudo systemctl daemon-reload

# 启动服务
sudo systemctl start smtps-server

# 设置开机自启
sudo systemctl enable smtps-server

# 查看状态
sudo systemctl status smtps-server

# 查看日志
sudo journalctl -u smtps-server -f
```

### 2. 性能优化

#### 调整 TCP 参数

编辑 `/etc/sysctl.conf`:

```conf
# TCP 优化
net.ipv4.tcp_fin_timeout = 30
net.ipv4.tcp_keepalive_time = 300
net.ipv4.tcp_tw_reuse = 1
net.core.somaxconn = 4096
net.core.netdev_max_backlog = 5000
```

应用配置：

```bash
sudo sysctl -p
```

#### 调整文件描述符限制

编辑 `/etc/security/limits.conf`:

```conf
* soft nofile 65536
* hard nofile 65536
```

### 3. 日志管理

配置日志轮转（logrotate）：

创建 `/etc/logrotate.d/smtps-server`:

```
/path/to/mail-system/v6/logs/*.log {
    daily
    rotate 7
    compress
    delaycompress
    missingok
    notifempty
    create 644 your-username your-username
}
```

### 4. 监控和告警

#### 使用 monit 监控

```bash
sudo apt-get install monit
```

创建监控配置 `/etc/monit/conf.d/smtps-server`:

```conf
check process smtps-server with pidfile /var/run/smtps-server.pid
    start program = "/usr/bin/systemctl start smtps-server"
    stop program = "/usr/bin/systemctl stop smtps-server"
    if failed port 465 protocol smtp then restart
    if 5 restarts within 5 cycles then timeout
```

---

## 常见问题

### 1. DNS 解析失败

**问题**: `nslookup mail.hgmail.xin` 返回错误

**解决方案**:
- 检查 DNS 记录是否正确配置
- 等待 DNS 传播（最多 48 小时）
- 使用 `dig` 命令检查权威 DNS

### 2. SSL 证书无效

**问题**: `openssl s_client` 报告证书错误

**解决方案**:
- 检查证书文件路径
- 确认证书未过期
- 验证证书域名与实际域名匹配
- 检查证书链是否完整

### 3. 端口无法访问

**问题**: `telnet mail.hgmail.xin 465` 连接超时

**解决方案**:
- 检查防火墙是否开放端口
- 检查云服务商安全组配置
- 确认服务器进程正在运行
- 检查服务器是否监听 0.0.0.0

### 4. 邮件被 QQ 邮箱拒收

**问题**: 邮件发送成功但 QQ 邮箱未收到

**解决方案**:
- 检查垃圾邮件箱
- 配置 SPF、DKIM、DMARC 记录
- 确保邮件内容符合规范
- 检查 IP 是否被列入黑名单

### 5. 数据库连接失败

**问题**: 日志显示数据库连接错误

**解决方案**:
- 检查 MySQL 服务是否运行
- 验证数据库配置（host、port、user、password）
- 检查数据库用户权限
- 测试数据库连接：`mysql -u mail_test -p -h localhost mail`

### 6. 内存占用过高

**问题**: 服务器内存使用率持续上升

**解决方案**:
- 调整连接池大小
- 增加系统内存
- 优化邮件处理逻辑
- 检查是否有内存泄漏

---

## 附录

### A. DNS 记录详细说明

| 记录类型 | 用途 | 示例 |
|---------|------|------|
| A | 域名到 IP 映射 | `mail.hgmail.xin → 123.45.67.89` |
| MX | 邮件交换记录 | `@ → mail.hgmail.xin` |
| TXT | SPF 防垃圾邮件 | `@ → "v=spf1 mx ~all"` |

### B. 端口说明

| 端口 | 协议 | 用途 |
|------|------|------|
| 25 | SMTP | 标准 SMTP（无加密） |
| 465 | SMTPS | SMTP over SSL/TLS |
| 587 | Submission | STARTTLS（可选） |

### C. SSL 证书续期提醒

Let's Encrypt 证书有效期为 90 天，建议设置自动续期。Cron 任务会每周检查一次，如果证书即将过期（30 天内），自动续期。

### D. 邮件服务器反向 DNS (PTR)

为了提高邮件送达率，建议配置反向 DNS 记录（PTR 记录），将 IP 映射到域名。联系你的云服务商或 ISP 配置。

---

## 总结

通过本指南，你应该能够：

✅ 配置域名 DNS 记录
✅ 申请和配置 SSL/TLS 证书
✅ 编译和部署 SMTPS 服务器
✅ 使用 QQ 邮箱测试邮件发送
✅ 优化服务器性能和稳定性

如有问题，请参考 [常见问题](#常见问题) 或查看 [日志文件](../README.md#查看日志)。
