# 运维与发布操作手册

当前生产标准（2026-10-18）：在本地与服务器 ABI 一致的 Linux amd64 容器中编译和链接，
服务器只收成品二进制。仓库中的 `deploy.sh` / `link.sh` 为历史 object-only 路径，
不再作为生产首选；不要将 Homebrew 头文件和 Ubuntu 运行库混用。

实际地址/凭据仅保存在未跟踪的 `docs/local/server-credentials.md`。本文使用占位符。

## 进程与主机角色

| 主机 | 服务 | 部署内容 | 数据 |
| --- | --- | --- | --- |
| 主站 | smtpserver / imapserver | smtpsServer + imapsServer 成对升级 | 权威用户、邮件、正文、附件 |
| 出站边缘 | smtpserver | smtpsServer | 独立中继 outbox/正文，不复制主站用户库 |
| 可选 Webmail | 单独装配 | webmailServer + build-webmail | 共享权威会话库/账号路由/正文存储 |

OutboundServer 嵌在 SMTP 和默认 DB 模式的 Webmail 进程，没有独立 outbound 二进制。
升级出站边缘的 SMTP 即升级其外投内核、MX 调度、签名和租约处理。仅更新主站不能覆盖边缘。
主站 systemd WorkingDirectory 通常为 `/opt/smtpServer`，实际入口/config 以
`systemctl show -p ExecStart -p WorkingDirectory` 为准。独立 IMAP 配置不是 SMTP 的 listener。

## SSH 与网络不佳时的跳板

出站边缘 SSH 已改为 **2222**；不要与 SMTP 中继监听端口混淆。优先使用阿里云主站作跳板：

```bash
# 本地有目标机私钥时，SSH ProxyJump
ssh -J root@<MAIN_HOST> -p 2222 root@<RELAY_HOST>

# 目标机密钥只配置在主站时，使用主站已有密钥；不复制私钥、不需要 agent forwarding
ssh root@<MAIN_HOST> 'ssh -p 2222 root@<RELAY_HOST>'
```

文件可以先传到主站暂存目录，再由主站 `scp -P 2222` 发到边缘（scp 的端口参数是大写 P）。
保留主机密钥校验；网络差时改用跳板，不关闭校验。只读检查先核对服务、版本、磁盘、内存、
DB schema 和 outbox 状态，再进入替换步骤。

## 本地容器构建

持久容器 `relay-build` 基于 `ubuntu:22.04`、`--platform linux/amd64`，源码挂载 `/src:ro`，
成品目录挂载 `/out`，构建目录在容器 `/build`。首次准备可用：

```bash
docker run -d --name relay-build --platform linux/amd64 \
  -v "$PWD:/src:ro" -v "$PWD/artifacts/linux-release:/out" ubuntu:22.04 sleep infinity
docker exec relay-build bash -lc 'apt-get update && apt-get install -y \
  build-essential cmake git pkg-config libssl-dev libspdlog-dev libc-ares-dev \
  libmysqlclient-dev libboost-dev libboost-system-dev libboost-thread-dev \
  nlohmann-json3-dev libcurl4-openssl-dev && git config --global --add safe.directory /src'
```

使用 `achieve=mariadb` 时另装 MariaDB Connector/C 开发包并确认 CMake 检测成功；
不要只修改 DB 配置而部署一个没有该驱动的二进制。生产 `achieve=mysql` 不要求该可选驱动。

```bash
docker exec relay-build bash -lc 'cmake -S /src -B /build \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_OBJECT_ONLY=OFF -DBUILD_TESTS=ON \
  -DENABLE_HDFS_WEB_STORAGE=ON -DENABLE_S3_STORAGE=ON \
  -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -march=x86-64-v2" && \
  cmake --build /build --target smtpsServer imapsServer -j4 && \
  mkdir -p /out/release && cp /build/smtpsServer /build/imapsServer /out/release/'
```

必须显式使用服务器 CPU 支持的 ISA，旧边缘 CPU 不含 AVX2。不要在小内存服务器编译/链接。
构建前记录工作区是否干净，产物 `--version` 应有真实 commit，不能接受 `Commit: unknown`
而声称已核对版本。用 `sha256sum`/`shasum -a256` 留下两份成品指纹。

## 发布检查与迁移

- 本地 CTest、前端构建/格式和真实浏览器 fixture 通过；发布 SMTP 内核同时检查 IMAP，避免版本漂移。
- 对照迁移检查 subject 长度、mailbox_uidnext、mail_aliases、quota 和当前代码引用的表/列。
  只应用缺失且已审阅的迁移，备份相关表。Webmail 会话迁移只在部署 Webmail 时需要，
  多会话表迁移必须先停旧 Webmail 写入节点。
- 上传到独立 release 暂存目录，先运行 `ldd` / `--version` / 校验和；不覆盖生产配置、
  数据库凭据、证书、DKIM 私钥、mail 或 attachments。不要对这些目录使用 rsync --delete。
- 主站备份 SMTP/IMAP 后成对替换；边缘只替换 SMTP。保留上一版本用于快速回滚。

## 原子替换、冒烟与回滚

同一文件系统内先上传成 `smtpsServer.new` / `imapsServer.new`，备份现役，再 `mv`。
运行中的进程继续持有旧 inode，重启后使用新文件。不要直接 cp 覆盖运行中的可执行文件。

```bash
# 服务器上执行；STAMP 必须标识本次发布并与校验记录对应
cd /opt/smtpServer
cp -p smtpsServer "smtpsServer.bak-$STAMP"
cp -p imapsServer "imapsServer.bak-$STAMP"
mv smtpsServer.new smtpsServer
mv imapsServer.new imapsServer
systemctl restart smtpserver imapserver
systemctl is-active smtpserver imapserver
./smtpsServer --version
./imapsServer --version
```

检查本机监听、SMTP EHLO、465/587 TLS+AUTH、IMAPS 登录/SELECT/FETCH、outbox 结果及
新日志。测试失败立即回滚：先把备份复制到另一个临时路径，再原子 mv 回正式路径并重启；
不要 cp 覆盖正在执行的 inode。首次部署没有备份时不能声称具备自动回滚。

## 附件与实际送达验收

生成已知 SHA-256 的文本/二进制附件，经真实 SMTP 发送，IMAP FETCH 原始 MIME，
使用独立 email 解析器解码并核对文件名、长度和哈希。必要时验证 BODYSTRUCTURE/section
与附件存储元数据。外域投递再核对边缘队列、对端 SMTP 最终响应及收件方实际附件。

对端 250 只证明对方接受，不证明进入收件箱；收件箱/垃圾箱归类须由收件人确认。
个人 QQ 的一两次成功不能替代其他提供商、企业网关和长期运行的送达验证。

```bash
journalctl -u smtpserver -u imapserver --since '10 minutes ago' --no-pager
systemctl show -p MainPID -p ExecStart -p WorkingDirectory smtpserver imapserver
```

认证日志、邮件正文、收件人地址、生产凭据和 DKIM 私钥不能作为公开发布记录提交。
公开记录保留版本、检查项目和脱敏结论；真实地址/测试凭据放未跟踪的本地运维记录。
