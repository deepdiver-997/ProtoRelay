# IMAP FETCH 多个 BODY section 漏返

手机客户端收取附件测试邮件失败。服务器日志显示认证成功，随后客户端请求
`UID FETCH <uid> (BODY.PEEK[2] BODY.PEEK[1])`，再关闭 TLS 并重试。
独立 Python imaplib 复现同一请求：tagged OK，但响应只有 BODY[2] literal，缺少 BODY[1]。
因此单独 BODY[] 或单独 BODY[n] 的既有验收不能覆盖真实手机请求。

原因是 FETCH 解析只查第一个方括号，将单个节号保存在 FetchContext；组装时也只输出
一个 section。后续还会把完整 MIME 字符串覆盖为选中节内容，混合请求的 BODYSTRUCTURE
可能受到影响。

修复保留所有顶层数字 section/完整 BODY[] 请求及其顺序，从同一份原始 MIME 取各段，
并保留原文供 HEADER/BODYSTRUCTURE 使用。非法/溢出数字不能通过 stoi 异常中止异步链。

专项 `imaps_fsm_test::fetch_body_part_number` 同次索取 2、1、完整 MIME 与 BODYSTRUCTURE，
消费者按每个 literal 的声明长度读取，分别比较 HTML、纯文本及完整原始邮件，检查顺序。
仅加入回归运行旧实现，因缺少第二个 literal 断言失败；修复后 imaps_fsm_test 全部通过。
最终真实客户端复验记录在
[发布验证](../reports/release-2026-10-18.md)。
