# IMAP 附件 BODYSTRUCTURE 字段与旧缓存

网易邮件大师已能读正文，却未列出附件。退出对话显示它只抓正文段，没有请求 PNG/CSV。
核对 RFC 3501 §7.4.2 后确认：附件名被写入 body-id 且是 list，disposition 一直为 NIL；
非 text part 还错误添加 text-only 的行数，导致字段位置偏移。中文 RFC 2231 filename*
以及连续分段也未解析，因此合法原始 MIME 能取回不等于客户端能识别附件。

修复按类型组装 BODYSTRUCTURE，NAME/FILENAME 参数和 Content-ID 各归正确字段，
保留 attachment/inline；非 ASCII 参数用 IMAP literal，引用串转义引号/反斜杠。
MIME 参数统一展开折叠头、处理 quoted-pair 与 RFC 2231 单段/连续段，保留大小写。
未知非 UTF-8 字符集通过 encoded-word 保留编码信息交给客户端，不误当 UTF-8。

派生 sidecar 增加 v=2 及 disposition/Content-ID。旧格式仍可读取；在需要完整元数据时
从原 MIME 懒重建并原子回写，包含只取 BODYSTRUCTURE 的快路径，不删除用户邮件、
不修改 UID/UIDVALIDITY，不要求数据库迁移。没有原文时不伪造新缓存。

专项使用独立 IMAP list/string/literal 消费者，按字段位置识别 text 与 image 附件，
比较中文分段文件名、inline、Content-ID，并验证旧 sidecar 重建。旧实现 7 项失败；
修复后 221 项断言通过；线上标准客户端验收见 [发布验证](../reports/release-2026-10-18.md)。
IMAP 直接从完整 MIME 提取 section，不依赖 Webmail 的独立附件文件/索引；两条路径
需分别验收。客户端可能缓存旧 BODYSTRUCTURE，复验使用新邮件，不改变 UIDVALIDITY。

参考：[RFC 3501 §7.4.2](https://www.rfc-editor.org/rfc/rfc3501#section-7.4.2)、
[RFC 2231](https://www.rfc-editor.org/rfc/rfc2231)。

补充验证：初版 ASan+UBSan 下 196 项通过。独立 LLVM 23 fuzz 构建同时发现实体头依赖
传递包含 <memory>，已显式包含，保持 MIME fuzz 不依赖 SMTP/HTTP 或 OpenSSL 链接。

模糊测试另发现同名 boundary 的畸形兄弟 part 会在未限区间时被重复当作后代，深度封顶
仍有指数级工作量。已把递归输入限制在当前 part 边界内，保持绝对 offset；24 个重复
boundary 的最小复刻在旧版单输入 2 秒超时，修复后立即完成，221 项断言通过。
原始触发输入和最小复刻都通过修复后 sanitizer 单输入验证，最小种子纳入 MIME fuzz corpus。
独立 IMAPClient 在修复前的生产 BODYSTRUCTURE 中识别到 0 个附件，期望为 3，作为客户端侧负验证。
