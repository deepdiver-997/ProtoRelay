# SessionBase 未成行输入停止读取

## 现象和原因

生产附件验收补测 Python imaplib 的 AUTHENTICATE PLAIN 时，服务器发出 `+ ` 挑战，
客户端已发送认证响应，但双方等待到超时。相同账号 IMAP LOGIN、手工单次发送一整行
和 SASL-IR 均成功。

探针仅记录发送长度与服务端状态，不记录认证内容，确认 imaplib 将 base64 内容和 CRLF
分两次 `send`。SessionBase 首次读取后没有完整行，未调用协议 handler；默认
`auto_continue_read=false` 的响应驱动协议也没有响应写完成回调触发下一次读取，
因此后续 CRLF 一直留在 socket 中。该问题不限于 SASL，普通 IMAP 命令也可能分片。

## 修复边界

记录本次 read 是否分发过完整输入。未分发、未暂停且未关闭时继续读取；已分发命令
仍遵守原协议的 auto_continue_read/响应完成回调约定，不额外预读。
缓冲上限、暂停、关闭和流水线消费逻辑保留。

## 双向回归

`session_base_test` 用 deferred MockConnection 依次交付认证行内容、CR、LF：

- 前两次没有完整行，必须保留下一次 pending read，不能提前调用消费者。
- LF 到达后消费者恰好收到一次完整行，处理次数为 1，缓冲为空。
- 响应驱动协议不因修复而在完整命令之后额外发起读。

仅加测试、未修复时运行旧实现：`pass=59 fail=4`，测试退出失败；加入修复后 `pass=63 fail=0`，退出成功。
真实生产客户端互操作与最终构建结果记录于 [发布验证](../reports/release-2026-10-18.md)。
