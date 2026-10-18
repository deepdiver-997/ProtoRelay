# Webmail 共享会话与进程内提交服务

2026-10-18。接续 `smtp-client-core.md` 和 `persistence-completion.md`。

## “共享账号政策”的含义

SMTP 和 Webmail 共用 `AccountPolicy` 的认证口径、认证身份绑定发件人、外域投递
开关、别名展开和每日配额逻辑。它不是多人共用一个登录账号，也不是别名的另一种
叫法；别名/邮件组只是其中一项规则。SMTP 的 session pause 与回复仍留在 FSM。
Webmail 的 HTTP 状态映射留在 API/DirectSubmitter；公共服务不持有 SMTP server。

## 每个账号只保留一个当前会话

用户已确认“防多地登录”：**新登录踢掉旧会话**。生产配置 `use_database=true`
时仍使用独立 `web_sessions` 表，而不是 `users.token`。每次成功登录生成新的
256 位 CSPRNG token，cookie 持有原 token，数据库只持有 SHA-256 摘要。
`(account_shard,user_id)` 唯一键保证每个账号只有一行当前会话；登录通过一条
原子 upsert 替换 token、凭据摘要和到期时间，提交成功后才发送 cookie。
不会先独立删除旧记录再插入。

多个节点并发登录由 InnoDB 唯一键串行化，以 **DB 成功写入顺序** 为准，最后一次
写入的 token 唯一有效。并发 HTTP 响应的到达顺序不代表登录先后，较早一次成功
响应所带 cookie 可能已被另一次登录替换。旧 cookie 后续在任意节点访问 me、
邮箱或发信均返回 401；固定 TTL 到期和新会话注销都不会恢复旧 token。
普通 logout 按 token 摘要删除：被踢的旧设备再注销，匹配不到新 token，不能
误删新会话。错误密码不轮转会话。进程重启保留当前会话，不同账号及不同 shard
上重复的用户 ID 各自独立。内存测试后端遵循相同的单会话策略。

集中会话表位于路由器的 **shard 0**，行记录账号地址、用户 ID、账号 shard、到期
时间和凭据版本摘要。用户本身仍在账号 shard。注册表没有跨库外键。查询先释放
注册表连接，再借账号库连接，避免单连接池重入。

每次受保护请求查权威 DB：token 存在、未到固定 24h TTL，再核对账号存在、ID、
status=1 以及当前密码哈希的 SHA-256 是否与签发时相同。没有会话正缓存，因此
跨节点 logout/改密没有 TTL 缓存窗口。禁用期间拒绝所有旧会话；**重新启用而未
改密时，尚未注销且未过期的会话可以恢复使用**。注销不撤回已经开始处理的请求。
SQL 使用 UTC，时限由 DB 时钟判定。成功登录顺带按过期索引清理最多 128 条旧行。

Web 登录走 fresh 认证，不使用 SMTP 的短期 AuthCache 命中结果；历史密码仍走
公共验密/rehash CAS。rehash 后再次读取凭据，CAS 被并发改密抢先时重新验密，
不能给旧密码签发新会话。凭据摘要不是明文密码，会话表和生产会话均不保存用于二次 SMTP
认证的密码。密码哈希发生变化（包括格式升级）会使先前会话失效。

DB 故障返回 503，未知/过期/已撤销 cookie 返回 401；不会降级为节点本地会话。
logout 写 DB 失败返回 503，不声称已注销。内存 SessionStore 和 LoopbackSmtpSubmitter
继续作为显式注入的兼容/测试适配器，只有该旧组合可以保留临时 SMTP 密码。

“多地”既可以是用户在不同设备/地域登录，也可以是多个服务节点。多个节点必须
连接相同权威会话库并保持一致的账号路由和共享正文存储；异步只读副本不能保证
即时撤销，本文不承诺跨地域复制/故障切换。固定 TTL、HttpOnly、SameSite=Lax；
现有 cookie Secure/TLS 感知装配仍待补，公网应在 HTTPS 入口部署。

## 提交与持久化

默认 Webmail 发信路径：

```
HTTP session 校验 → MIME compose → DirectSubmitter
  → MailSubmissionService（actor ID/address 再校验）
  → AccountPolicy（配额、外域开关、别名展开、目标去重）
  → worker 上写正文并 commit → PersistentQueue
  → 事务提交/持久化完成通知 → HTTP 202
```

服务通过注入的路由、worker、AccountPolicy、PersistentQueue 和 ExecutorProvider
工作；没有 SMTP socket、SMTP server 指针、用户密码、MX 或投递租约逻辑。
正文仍走 IStorageProvider/MailBodyWriter，不另造存储或事务组件。未入队的已提交
正文在失败时清理；入队后所有权交给 PersistentQueue。MIME sidecar 失败记录日志，
读信仍可惰性解析。等待 deadline 使用 `inbound_persist_wait_timeout_ms`。
超时返回 504/unknown，不取消已入队工作、不自动重试；持久化仍可能完成。
202 始终表示确认持久化，与 Webmail 配置的 `inbound_ack_mode` 无关，且不表示外域
实际送达。没有自动 Sent 副本、幂等键或配额补偿；这些继续后置。

配额保留 SMTP 的既有语义：一次 MAIL/提交占用一次配额，后续拒绝或存储故障不
返还；连接或 UPDATE 故障时配额检查 fail-open 并记 WARN；ROW_COUNT 查询/解析失败
按未占用拒绝（均沿用 SMTP 旧语义，身份校验本身仍 fail-closed）。
提交路径非别名/别名查询失败回退为原收件人，与 SMTP 兼容。别名缓存 TTL 300s。
改这些业务语义需要后续独立阶段，不能误认为已有严格幂等和端到端恰好一次。

应用装配创建队列和 OutboundServer，出站仍用已有 outbox/MX/租约/DKIM/重试；
SMTP 与 Webmail 共用 OutboundConfig 构造。多个出站实例通过 DB 租约协调。
同一 shard 的本地文件正文目录须对所有读/出站节点可见，或配置共享存储后端。
新可信 listener、独立服务身份认证/TLS 长连接协议尚未实施，公开 25 策略不变。

## 迁移、配置与验证

新建库在 **shard 0** 运行 `config/sql/migration_web_sessions.sql`；两份建表脚本
同样包含账号唯一键。已部署上一版多会话表时，**先停止旧 Webmail 写入节点**，
再运行 `config/sql/migration_web_sessions_single_login.sql`，随后一起升级服务。
迁移按账号保留 created_at 最新一条，同秒时按 token 摘要排序决出一条，删除其余
会话，并把旧普通索引替换为账号唯一键；可重复执行。被清掉的旧设备需要重登。
无须在 users 添加 token 列，也不改账号或邮件数据。新登录在写入前核对唯一键，
缺少迁移、索引不正确或元数据查询失败时返回 503，不签发可能共存的新会话。

服务应定期清理闲置环境中过期行，例如 `DELETE FROM web_sessions WHERE
expires_at <= UTC_TIMESTAMP() LIMIT 10000`；正常登录也会小批清理。

`webmail_node_id`：整数 0..1023，默认 1；用于独立 Snowflake 生成器，低 5 位
worker、高 5 位 datacenter。**所有写同一邮件库的进程必须使用不同 ID**，包括
SMTP 默认节点 0；多 Webmail 节点分别配 1、2 等。当前 SMTP 全局默认 ID 为 0，
其多实例 ID 装配仍属旧限制。不能把默认示例原样复制到多个写节点。
`webmail_submission_port` 已由默认直接提交替代，不再读取；HTTP 无需连接 587。
发信节点现在也需配置存储、外域开关、配额和完整 outbound 参数，需与 SMTP 的
策略一致（不再借 SMTP 进程的配置）。入口 `--help` 和示例配置已同步。

```bash
cmake --build build-release -j 8
ctest --test-dir build-release -j 4 --output-on-failure
.venv/bin/python test/scripts/webmail_send_smoke.py --db-config config/db_config.json
.venv/bin/python test/scripts/webmail_single_login_migration_test.py --db-config config/db_config.json
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-asan -R '^(webmail_test|smtps_fsm_concurrency_test)$' --output-on-failure
```

会话标识在认证后具有凭据效力，服务端失效与注销原则可参考
[OWASP Session Management Cheat Sheet](https://cheatsheetseries.owasp.org/cheatsheets/Session_Management_Cheat_Sheet.html)。

本地 smoke 创建两个 Webmail、一个 SMTP 临时进程和随机账号，测跨节点互认、
新登录互踢、8 个并发登录仅一有效、旧设备注销保护、节点重启、跨节点发信/读信、
跨节点注销、禁用/到期/改密、别名去重及
SMTP/HTTP 共享配额；无外域邮件。账号、别名、会话、邮件和临时目录均自动清理。
故障状态和 stale-cache/rehash-CAS 竞争由 webmail_test 的 MockDb 场景覆盖。

单会话专项负验证、迁移去重/重跑验证见
[单会话回归记录](../../bugfixes/2026-10-18-webmail-single-login.md)。
