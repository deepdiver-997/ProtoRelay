# IMAP BODY[n] 恒空：FETCH 节号解析 substr off-by-one

日期：2026-09-28　|　影响面：imapsServer（全部 BODY[n]/BODY.PEEK[n] 取正文）　|　状态：已修复部署

## 症状

- 2026-09-25 perf 重构（`89449fa`）合入后，客户端按部分取正文（`BODY.PEEK[1]`/`[2]`）
  拿不到内容：响应缺 BODY[n] 数据项，客户端判"content 为空"并 2~5 秒一次无限重试
  （journal 里同一条 FETCH 每隔几秒重复、随后 stream truncated）。
- `BODY[]`（整封）、`BODY[HEADER]`、`BODY[HEADER.FIELDS ...]` 均正常 —— 只有数字节号死。

## 根因

`traditional_imaps_fsm.tpp` FETCH 属性解析（89449fa 引入）：

```cpp
std::string num_str = attrs.substr(bracket + 1, close - bracket);   // 多算了 1
```

`substr(pos, count)` 的长度把闭合 `]` 包了进来：`BODY.PEEK[2]` → `num_str = "2]"` →
`all_digits` 恒 false → `body_part_num` 恒 0 → `want_body` 不置位 → 服务器不取正文、
响应里没有 BODY[n] 项。

## 修复与测试

- `close - bracket` → `close - bracket - 1`。
- 回归测试 `imaps_fsm_test/fetch_body_part_number`：种子 multipart 邮件（无 sidecar，
  走 ensure_mime_tree 解析回写路径），对 `BODY.PEEK[1]`/`[2]` 断言响应含
  `BODY[1] {16}` + 明文内容、`BODY[2]` + html 内容。
  测试基建注意：mock DB 结果是队列（第二个 session 要再 push 一份）；异步 FETCH
  完成后 mock 不重新武装读路径（每 session 一条命令）。

## 排查记录（为什么不疑 MIME 解析）

Gmail 来信（见 `2026-09-28-gmail-inbound-starttls-rst-plaintext-fallback.md`）入库完好：
原始文件 4299 字节、头部确实约 4KB（Gmail 的 ARC/DKIM/X-Gm 头）、边界偏移
4076/4172/4267 与 .mime 树完全吻合，`z=15/15` 精确等于两部分的正文长度 ——
SMTP 侧入库、MIME 解析、sidecar 全部正确，问题在消费端 FETCH。

## 遗留

- `qt@scut.email`/`test@scut.email` 用文档密码登录失败：users.password 列存的是明文
  （"qtest123"/"2023…"），认证代码支持明文等值比较，按理应过；待查（auth cache/分片路由？）。
  `test3@scut.email`、sakura、deepdiver-997 登录正常。
