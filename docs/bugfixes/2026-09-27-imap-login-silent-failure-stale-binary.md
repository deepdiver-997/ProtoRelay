# IMAP「登录失败」实为响应丢失 —— 线上二进制版本漂移 + 连接池慢性泄漏 (2026-09-27)

- 日期：2026-09-27
- 类别：部署版本漂移 / DB 连接池泄漏 / pipelining / 静默失败
- 严重度：**pipelining 客户端（真实 MUA）IMAP 登录必败（服务端日志却全为 success）**；
  全体 DB 依赖操作退化到秒级
- 修复：无需改代码——重启清零泄漏状态后立刻恢复；根治 = 部署含 bf29fe8 的 HEAD 二进制

## 现象

用户报 Sakura 账号 IMAP 登录失败，怀疑服务端解析回归（此前有过一次解析 bug 导致
客户端主动退出）。排查发现三组互相矛盾的事实：

1. 服务健康：143/993 监听、TLS 握手正常、账号 status=1、bcrypt 哈希正常；
2. 日志里 **Sakura 登录全部 successful**（user_id=15），48 小时零条 failed；
3. 用户客户端仍持续报登录失败，且同一出口 IP 有每 ~7 秒一轮的 4 账号重试风暴。

## 定位过程（关键实验）

| 实验 | 结果 | 结论 |
|------|------|------|
| 外部单命令 LOGIN（带/不带引号） | 成功 | 密码与解析无问题 |
| 外部 pipelining：LOGIN+SELECT+SEARCH 一个 TCP 段 | **0 字节返回（100% 复现）**，服务端日志却含 `login successful` + `UID subcmd done` | 响应在服务端已产生、未上 wire |
| 同场景去掉 SEARCH / 换 LOGOUT 收尾 | 正常 | 多跳异步级联才触发 |
| SessionBase 级单线程 mock 复刻（pipeline_deferred_test） | 11/0 通过 | 代码逻辑无辜 |
| **重启 imapserver 后重跑同一 pipelining 场景** | **28ms 全量返回** | **状态性退化，非逻辑 bug** |

「重启即愈 + 确定性复现 + 单线程逻辑无罪」→ 指向运行态资源。对照时间线：
线上 imapsServer 二进制文件日期 **2026-09-14 02:10**（进程 9-24 启动、文件 9-14 构建），
而 bf29fe8「CPS 链回调漏捕 ScopedConnection + 池归还 tripwire」提交于 **9-24 18:21**
——**线上二进制比泄漏修复早 10 天**。9-25~9-27 的三轮部署只换了 smtpsServer，
imapsServer 一直没动。

## 根因链

1. 旧二进制的 CPS 链每次 DB 查询泄漏一个池连接（bf29fe8 修的正是这个）；
2. 跑 13 天池子见底 → 每个 DB 操作从毫秒退化到秒级（日志可见 LOGIN→SEARCH 起步间隔 2.85s）；
3. 真实 MUA pipelining 三跳级联（LOGIN→SELECT→SEARCH 各一次 pause+worker+DB）总时长
   超过客户端超时 → 客户端断开；
4. 完成回调里 `if (self->is_closed()) return;` 早退 → **积累在 pending_write_buf_ 的
   全部响应（含成功的 LOGIN OK）永远不出 wire** → 客户端视角=「登录失败」；
5. 单命令客户端每跳虽慢但能熬过超时 → 一直「正常」，形成真假混合的现场。

服务端日志全 success、零 failed 的原因：认证确实成功了，失败的是「响应送达」。

## 修复与验证

- 容器管线构建 HEAD imapsServer（含 bf29fe8 泄漏修复 + a7a653b O_NONBLOCK +
  89449fa 邮箱列表缓存）部署阿里云，旧二进制备份 `imapsServer.bak-prehead-*`；
- 四场景复测：pipelining 三命令 109ms / 加 LOGOUT 83ms / 单登录登出 74ms，全部完整返回；
- `pipeline_deferred_test`（c303d1d）锁定「pause + worker 延迟完成」级联的 wire 语义：
  pipelining 时全部响应必须最终落线；
- 顺手确认 `last_login_time` 自 9-24 起停更同属旧二进制问题（待观察新版是否恢复）。

## 教训

1. **部署必须成对**。SMTP 与 IMAP 共享大量 framework/DB 代码，只换一个二进制必然
   版本漂移。本次事故的直接原因就是「修 SMTP 顺路忘了 IMAP 还在跑 10 天前的代码」。
   手动部署后必须核对两端二进制的构建来源（`sha256sum` + 启动时间 vs 最近提交时间）。
2. **「重启即愈」是状态性退化的指纹**，第一反应查：连接池/内存/fd/线程数随时间的单调
   变化，以及**线上二进制到底包含哪些提交**——先 `ls -l` 看文件日期对比 git log，
   再谈代码 bug。
3. **服务端日志的 success 不等于客户端收到**。pipelining 下响应可能积累后统一 flush，
   「处理成功 + 客户端超时」的组合会把故障伪装成「客户端问题」。复现时要抓
   客户端视角的字节（本次用裸 socket 计时收包）。
4. **真假混合现场先找「哪些客户端活、哪些死」的判别条件**（本次：单命令活、
   pipelining 死），比逐个排除快得多。
