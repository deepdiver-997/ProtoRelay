# Gmail 收信失败根因实锤与修复：跨境 STARTTLS 被链路注入 RST → 25 端口可关通告回落明文

日期：2026-09-28　|　影响面：smtpsServer（25 端口入站）　|　状态：已部署缓解，待 Gmail 侧验证

## 症状与历史

- 2026-08-12 起 Gmail → `test3@scut.email` 持续失败（见 `docs/mail-system/bugfixes/2026-08-15-gmail-delivery-fix.md`）。
  当时定性为"IP 信誉（PTR 缺失 + Barracuda RBL）"，**该结论解释不了 TLS 阶段的 RST，本文修正**。
- 09-22 之后服务器日志里 Gmail 连接消失，用户 09-28 上午发的 Gmail 测试邮件未到达（DB 无记录、日志无连接）。
- 期间 OpenAI（SendGrid 通道）等境外发件人 09-23~26 一直能正常投递 —— 失败是来源选择性的。

## 抓包实锤（2026-09-28，RackNerd → 阿里云 25 端口）

明文 SMTP 全程正常（220 → EHLO → 250-STARTTLS → STARTTLS → 220 Ready），
ClientHello **从未到达服务器**，取而代之的是两个伪造 RST：

```
In  107.174.127.252.36944 > 10.0.0.166.25: Flags [R], ttl 55, id 35121
In  107.174.127.252.36944 > 10.0.0.166.25: Flags [R], ttl 55, id 35121   ← 同 IP id 发两次
```

- 客户端真实包 ttl=48、id 递增（42854…）；RST 包 ttl=55、id 相同 → 链路中间设备**伪造源地址、
  同时掐断双向**，注入点距服务器约 9 跳（靠近国内一侧的跨境链路上）。
- 服务器日志同款签名：`Handshake failed: Connection reset by peer (ec=104) … STARTTLS handshake failed`。
  与 Gmail 退信文本 "TLS negotiation failed: connection reset by peer" 互为印证。
- 08-15 复盘 tcpdump 观察到的 "ClientHello 后 53μs RST" 即此注入（近端注入，非服务器自身）。

## 排除项（本轮新证据）

- **服务器代码无辜**：同进程自测（对公网 IP）587 STARTTLS 全握手成功、465/993 隐式 TLS 正常、
  国内 465/993 TLS 正常 —— 只有"跨境 + TLS"组合死。
- **RackNerd 无辜**：同机连百度 443、连 Google aspmx:25 STARTTLS 均成功。
- 入侵拦截无辜：`/tmp/logs/intrusion_data.json` 为 null，ufw 规则正常放行 25。
- 注入是来源选择性的：被掐名单含 Gmail(74.125.x, 26 次)、RackNerd 中继(4 次)、Amazon SES、
  OpenAI 自有 MTA(o21.ptr7811.openai.com)；SendGrid(149.72.x) 能投成功（TLS 失败后回落明文）。

## 为什么 Gmail 一直投不进来

Gmail 在 STARTTLS 握手失败后**只退避重试、不回落明文**；只要我们 EHLO 还通告 `250-STARTTLS`，
每次重试都死在同一注入点。SendGrid 类 MTA 会回落明文所以无感。09-22 后 Gmail 进入深度退避
（近两日仅 1 次尝试），并非完全放弃。

## 修复：`advertise_starttls` 每监听器开关

- `include/framework/server_config_base.h` — `ListenerConfig::advertise_starttls`（默认 true）+ 基类解析。
- `include/framework/server_config.h` — `MailListenerConfig::from_json` 解析（SMTP 实际路径）。
- `traditional_smtps_fsm.tpp` — EHLO 不再通告 STARTTLS（`if constexpr` Ssl 分支保留）；
  STARTTLS 命令在未通告时回 `502 5.5.1 STARTTLS not supported` 并保持会话可用（RFC 3207 §4）。
- 配置：`config/smtpsConfig.json`（仓库样例同步）25 端口加 `"advertise_starttls": false`。
  **只影响 25**；587（提交端口）继续通告 STARTTLS+AUTH，465 隐式 TLS 不受影响。
- 测试：`smtps_fsm_test`（EHLO 无 STARTTLS、STARTTLS→502 且 NOOP 正常）+ `server_config_test`
  （解析与默认值）。macOS Debug + 容器 ubuntu:22.04 双平台全绿。

## 部署与验证（2026-09-28）

- 容器管线 `relay-build` 增量构建 **双二进制**（09-27 版本漂移教训），服务器 stop→备份
  （`*.bak-plainadv-*`）→换→start，两服务 active。
- 冒烟：25 EHLO 无 STARTTLS；587 仍有 STARTTLS+AUTH；465 TLS1.3 正常。
- 端到端：RackNerd 明文 SMTP 全会话投递 `PLAINTEXT-PATH-PROBE-20260928` → test3 收件记录落库（未读）。
  该路径即此前被 RST 的路径。

## 遗留 / 后续

1. **Gmail 侧验证**：等 Gmail 下一次投递尝试（或用户新发一封触发即时尝试）。若 Gmail 的
   SYN 已被升级为直接丢弃（09-28 当天无 Gmail 连接），则明文通告也救不了，只能换入站 IP。
2. 代价：25 端口全量明文（含境内发件人）。可接受性由用户判断；回滚 = 配置删一行 + 重启（无需重编）。
3. 根源：该 IP 跨境 TLS 被选择性干扰，同机 443/udp Hysteria + WireGuard 是嫌疑触发源；
   长期方案是入站 MX 迁到干净 IP（RackNerd 直投不可行——它到阿里云的 TLS 同样被掐）。
4. 08-15 复盘的 PTR/Barracuda 结论与 TLS RST 无关，勿再按该方向排查投递失败。
