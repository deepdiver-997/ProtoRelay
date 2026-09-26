# DKIM 验签链路四连静默 bug —— mail-tester 7.8→10/10 (2026-09-25)

- 日期：2026-09-25（排查+修复同日完成，根因横跨出站/入站/DNS 三层）
- 类别：DKIM / 邮件送达率 / 静默失败
- 严重度：**外发邮件 DKIM 全部无效（QQ/Gmail 评分压低，疑似冷启动投递被静默丢弃的诱因之一）；入站对多段公钥域名的 DKIM 验证全部静默失败**
- 验证：mail-tester.com 报告 test-a9kxpex3z（7.8 分，双签名全 invalid）→ test-uggvw5aqf（10/10，dkim=pass）

## 背景

用户报告 QQ 邮箱收不到冷启动投递（垃圾箱都没有）。用 mail-tester 实测生产链路
（阿里云 465 AUTH → RackNerd:2525 信任中继 → MX 直投），得分 7.8，扣分核心：
**邮件带两份 DKIM-Signature 且全部 `dkim=fail reason="signature verification failed"`**。

## 四个 bug 与修复

### ① 出站签名输入多一个 CRLF（de76cca）

- **现象**：私钥与 DNS 公钥模数一致、bh 校验通过，但 opendkim/dkimpy 全网验签失败。
- **定位手法**（值得复用）：用 DNS 公钥对线上 `b=` 做 RSA 还原得到「签名器实际签的哈希」，
  与按仓库代码逐字节复刻的规范化输入哈希比对 → 完全一致（部署二进制=仓库代码）；
  再 hook dkimpy 内部 sha256 抓「验证器实际哈希的字节流」→ diff 仅差结尾 `\r\n`。
- **根因**：`canonicalize_header_relaxed()` 对所有行统一补 CRLF，签名器对
  DKIM-Signature 自身头也补了。**RFC 6376 §3.7：该头参与哈希时不含结尾 CRLF**。
- **修复**：`outbound_utils.cpp::build_dkim_header` 签名输入剔除末尾 CRLF。
- **闭环证明**：按修复算法同私钥重签同一封邮件 → dkimpy PASS。

### ② 双签名（配置修复，无代码改动）

- **现象**：第一封测试邮件含两份签名（t 相差 6 秒）。
- **根因**：阿里云出站中继跳的 `outbound.dkim.enabled=true`——违反了
  RackNerd 接入方案「签名只在边缘跳做一次」的设计。RackNerd 入库时签一次、
  出站又签一次。
- **修复**：阿里云 `outbound.dkim.enabled=false`（入站 `inbound_dkim_mode=hard` 不受影响）。

### ③ 入站去点填充只覆盖非流式分支（40d98cd）

- **现象**：入站日志 `DKIM bh mismatch: domain=ollama.com/amazonses.com`，两封**不同**
  邮件报相同的 got（后证实是同一封邮件的两份签名，exp 本就相同——先误判为「哈希常量化」，
  拉到存储报文做字节级比对才纠正）。
- **定位手法**：还原存储报文，把 `..` 开头行再剥一层点后 sha256 与签名 bh **完全一致**
  → 存储的 body 比签名者原文多一层 stuffing。
- **根因**：入站去点填充写在 `!context_.streaming_enabled` 分支里，而 `streaming_enabled`
  等价于「检测到 multipart」——**multipart 邮件整条流带着 stuffed 字节落盘**，
  `.` 行比签名者原文多一个点 → bh 必然失配。
- **修复**（三处）：`LineFolder` feed/flush 行级去填充（跨块安全，替代旧的逐 chunk
  剥点——旧实现行被 TCP 分块切开时会误剥）；`process_message_data` 行循环对 body 行
  去填充（流式附件/缓冲正文都覆盖）；smtps_session DATA 分支重构（终结符剥离先行，
  multipart 引擎喂干净字节）。回归：line_folder_test 8-11。

### ④ DNS TXT 多段记录不拼接（6e8703f）

- **现象**：修好 ③ 后，手工 dkimpy 签名的测试邮件入站**静默**失败——无任何 DKIM 日志。
  加结果日志探针后定位到 pubkey-empty 分支；本地按存储头块复刻解析器证明签名/tag
  均正常，锁定 DNS 层。
- **根因**：`txt_callback` 用普通版 `ares_parse_txt_reply` **逐 character-string 返回**。
  DKIM 2048 位公钥超 255 字节必然被 DNS 分成多段；`extract_dkim_pubkey` 取到截断的
  `p=`（非空），`d2i_PUBKEY` 失败且该 return false 路径**零日志**。
  单段短 key（qq.com）一直正常——所以 QQ 邮件的入站验证从未暴露此 bug。
- **修复**：改用 `ares_parse_txt_reply_ext` 按 `record_start` 拼接同记录多段
  （兼容 jammy c-ares 1.18 的 `ares_txt_ext` 命名）；pubkey-empty 分支补 WARN
  （DNS 回调对值捕获的 result 赋 reason 会丢失，不能依赖最终 reason）。

### 附带发现：STARTTLS handoff 丢 ListenerConfig（88f10c0）

排查 587 AUTH 通告缺失时发现：`handoff_starttls_socket` 给升级出的 TLS 会话传
`ListenerConfig{}` 默认值 → 587 STARTTLS 后 auth_policy 退化 OFF，EHLO 不再通告
AUTH（465 原生 TLS 不走 handoff 故未暴露），MAIL FROM 的 require_auth 判定同样失真。
修复：按 socket 本地端口从 `m_listener_configs` 反查真实配置。回归：starttls_handoff_test。

## 教训

1. **测试必须站在验证者视角**。旧 DKIM 测试只断言「wire 里有 DKIM-Signature 头」，
   四个 bug 全部漏网。修复后补的 3.9 用验证器算法独立重建签名输入 + EVP 验签，
   并对未修复代码验证「必 FAIL」（双向验证，防假绿）。
2. **静默失败路径必须可观测**。d2i_PUBKEY 失败、pubkey-empty、bh mismatch 早期全静默，
   排查只能靠黑盒探针。现在 DKIM 验证每个失败分支都有 WARN（含 key_domain/records 数）。
3. **「收下了」≠「送到了」**。QQ MX 250 收下后按评分静默丢弃不留痕——投递验证要看
   对端最终可见性（mail-tester 报告/对端自助查询），不能只看己方日志的 250。
4. **邮件字节级的比对是最快路径**。拉存储报文做 sha256/RSA 还原/diff，一步锁定差异字节，
   胜过反复读代码推演。
