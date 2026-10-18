# IMAP 异常会话追踪与认证标记

网易邮件大师取邮件失败后，用户要求保存所有非正常 LOGOUT 的会话。
核对代码与生产 trace：现有 SessionBase 已按此策略保存，与是否认证无关；
但 IMAP 直到析构才同步基座认证标志，close 时生成的文件误写 `auth: no`。
此外 trace 中 LOGIN 与 SASL 初始/续行凭据尚未脱敏，文件默认权限过宽。

本次保持正常 LOGOUT 丢弃、异常退出落盘的策略，并修正：

- 认证成功立即同步基座状态，trace/会话指标能看到真实认证结果。
- IMAP LOGIN 参数、AUTHENTICATE 初始响应与 PLAIN 续行在入缓冲前脱敏；
  入站 literal 仅记长度，防凭据或邮件内容经 literal 混入。
- 后续 UID/FETCH 等命令与服务端响应继续保留，单连接上限 64 KiB、目录上限 300 文件。
- trace 目录权限 0700，新文件权限 0600；保存失败输出可观测警告。

`imaps_fsm_test::abnormal_trace_after_auth` 实际走认证链后异常关闭并读取退出文件，
验证 `auth: yes`、无明文/可逆 base64 凭据、保留 FETCH 请求、文件仅属主可读写。
再正常关闭另一会话，确认不新增 trace。仅加回归运行旧实现：三个条件全为 false，
断言失败；修复后三个条件全为 true，IMAP 测试全部通过。历史文件不自动重写，运维限制目录/文件权限，不公开原始 trace。

文件位置是有效 `log_file` 的父目录下 `traces/`，不是固定工作目录。
STARTTLS 交接继续沿用已有 trace 移交机制，前后命令写入同一份退出记录。
