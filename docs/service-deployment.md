# ProtoRelay Service Deployment

ProtoRelay 使用前台进程模型运行，建议始终交给 systemd 或 launchd 托管。

## 1. 运行模型

不在进程内 daemonize/fork，主要原因：

- 与 Docker/Kubernetes/systemd 等现代调度器兼容更好。
- 信号处理、退出语义、崩溃重启都更清晰。
- 避免 pidfile/fork 带来的额外复杂度和隐蔽故障。

## 2. Linux systemd 部署

模板文件：

- `deploy/systemd/protorelay.service`

建议步骤：

```bash
sudo cp deploy/systemd/protorelay.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now protorelay
sudo systemctl status protorelay
```

查看日志：

```bash
journalctl -u protorelay -f
```

## 3. macOS launchd 部署

模板文件：

- `deploy/launchd/io.protorelay.server.plist`

当前用户安装示例：

```bash
mkdir -p ~/Library/LaunchAgents
cp deploy/launchd/io.protorelay.server.plist ~/Library/LaunchAgents/
launchctl unload ~/Library/LaunchAgents/io.protorelay.server.plist 2>/dev/null || true
launchctl load ~/Library/LaunchAgents/io.protorelay.server.plist
launchctl list | grep protorelay
```

## 4. TLS 证书获取与接入

ProtoRelay 读取配置中的 `certFile` 和 `keyFile`。以下给出三种常见方式。

### 4.1 开发环境: 自签名证书

快速生成（仅开发/联调使用）：

```bash
openssl req -x509 -nodes -newkey rsa:2048 \
	-keyout config/crt/server.key \
	-out config/crt/server.crt \
	-days 365 \
	-subj "/CN=mail.example.com"
```

然后在 SMTPS 配置中设置：

- `certFile`: `crt/server.crt`
- `keyFile`: `crt/server.key`

### 4.2 生产环境: Let's Encrypt

推荐由 Certbot 签发证书。常见路径：

- 证书: `/etc/letsencrypt/live/<domain>/fullchain.pem`
- 私钥: `/etc/letsencrypt/live/<domain>/privkey.pem`

可通过软链接暴露给 ProtoRelay 配置目录，或直接在配置中使用绝对路径。

注意事项：

- 续期后需重启服务加载新证书（当前版本未实现热重载证书）。
- 证书私钥文件权限应严格限制（建议仅服务用户可读）。

### 4.3 DKIM 密钥

项目内已提供脚本：

```bash
./scripts/generate_dkim_keys.sh
```

生成后将私钥路径配置到：

- `outbound_dkim_private_key_file`

并同步配置：

- `outbound_dkim_enabled: true`
- `outbound_dkim_selector`
- `outbound_dkim_domain`

同时把公钥 TXT 记录发布到 DNS（`<selector>._domainkey.<domain>`）。

## 5. 日志策略（重点）

配置项：

- `log_level`: `trace/debug/info/warn/error/critical/off`
- `log_to_console`: 是否输出到 stdout/stderr
- `log_to_file`: 是否输出到滚动文件
- `log_file`: 文件日志路径

### 5.1 systemd 托管推荐

推荐模式：

- `log_to_console: true`
- `log_to_file: false`

理由：

- 日志统一进入 journald，检索与轮转交给系统。
- 避免 journald 与本地文件双写导致重复与 IO 开销。
- 便于集中采集（如 rsyslog/Vector/Fluent Bit）。

### 5.2 非托管/本地调试推荐

推荐模式：

- `log_to_console: true`
- `log_to_file: true`

可同时在终端观察和留存文件。若日志太多，优先把 `log_level` 从 `debug`/`trace` 调整到 `info` 或 `warn`。

### 5.3 极简生产模式

若只保留文件日志：

- `log_to_console: false`
- `log_to_file: true`

适用于无 journald 的场景，但要自行做好文件轮转与收集。

## 6. 可选构建：禁用 WebHDFS/libcurl

如果不使用 `storage_provider=hdfs_web`，可在构建时禁用 WebHDFS：

```bash
cmake -S . -B build -DENABLE_HDFS_WEB_STORAGE=OFF
cmake --build build -j
```

或使用脚本：

```bash
EXTRA_CMAKE_ARGS='-DENABLE_HDFS_WEB_STORAGE=OFF' ./build.sh Release
```

禁用后若运行时仍配置 `storage_provider=hdfs_web`，服务会在启动阶段快速失败并提示原因。
