# 认证密码哈希：格式口径、缓存与 rehash-on-login

日期：2026-10-17　|　影响面：smtpsServer / imapsServer / pop3Server 认证链　|　状态：已实现

## 1. `users.password` 列的三种格式

| 格式 | 判定口径 | 来源 |
|------|----------|------|
| bcrypt | `"$2"` 前缀（`$2a$/$2b$/$2y$`，60 字符） | register-service / `hash_tool` 产出，**目标格式** |
| MD5 | 32 位**小写**十六进制 | 早期导入数据 |
| 明文 | 其它任意字符串 | 手工 INSERT / 临时重置 |

判定与校验统一在 `common/bcrypt.h`：

- `is_bcrypt_hash(stored)` —— 只看 `"$2"` 前缀，与 `bcrypt_verify` 口径一致
- `verify_password_hash(input, stored)` —— 按 stored 的格式自动选校验方式
- `password_needs_rehash(stored)` —— 非 bcrypt 即需升级

**统一之前的坑**：三个协议各自手写判断且口径不一——SMTP 认 MD5，IMAP/POP3 只认 bcrypt
与明文。同一个 MD5 账号能发信却登不上收信端，排查时会误判成"密码错了"。现在三条链都
走同一个函数，口径副作用也一致：明文密码本身恰为 32 位小写 hex 的账号会被当 MD5 校验而
登不上（既有 SMTP 行为，极罕见，不特殊处理）。

## 2. rehash-on-login：历史格式随登录逐账号收敛到 bcrypt

### 为什么不是一次性迁移脚本

MD5/明文不可逆，**只有用户登录这一刻手里才有明文**，才算得出 bcrypt。所以升级只能随登录
逐账号发生：没登录过的账号保持原样、照旧能登；登录过一次就永久变 bcrypt。

### 链路（`common/password_rehash.h::rehash_password_if_needed_async`，三协议共用）

```
auth SELECT (id, password, status)
  → verify_password_hash 通过
  → [stored 非 bcrypt] bcrypt_hash(明文) → UPDATE users SET password=? WHERE mail_address=? AND password=?
                        成功 → 刷新 AuthCache 条目为新 bcrypt
  → UPDATE last_login_time
  → 回调 235 / OK / +OK
```

### 契约（也写在头文件注释里，以头文件为准）

- **只在验密成功后触发**；错误密码绝不碰 DB。
- **CAS 回写**：`WHERE password = 旧值`。用户用旧密码登录中、管理员同时改了密码 → 影响 0 行，
  不会把新密码覆盖回旧值；下次登录按新密码走。缺了这条守卫，并发改密会被静默回滚。
- **任何失败只记日志、不影响本次登录**：bcrypt_hash 失败 / 无连接 / UPDATE 失败 → 历史行
  原样保留，下次登录再试。`LOG_AUTH_WARN` 带账号，可观测（规范 10.2）。
- **成功后刷新缓存条目**：否则明文/MD5 会在 `AuthCache` 里再躺一个 TTL（5 分钟）。缓存命中
  路径不升级——TTL 过期回源走 DB 路径时自然补上。
- **线程**：bcrypt_hash 几十~几百 ms 纯 CPU，三条链本来就在 worker 线程发起认证，回写顺带
  在同一线程完成；`done` 恰好一次，线程与 `conn->async_execute` 回调一致。

### 残余（有意接受，不是遗漏）

- CAS 落空时 `async_execute` 仍可能回 `ok=true`（0 行不算错误），缓存会被刷成**旧密码的
  bcrypt**。这与不做 rehash 时缓存里躺着旧 stored 等价——都受同一个 TTL 窗口约束，不新增
  暴露。要彻底消掉需再查一次 `ROW_COUNT()`，每账号一生一次的升级不值得多这一跳。
- 改密码对**已建立会话**无效（IMAP IDLE 长连接 / SMTP 会话级 `is_authenticated`），这是
  会话失效问题，与哈希格式无关，另案处理（会话注册表 + 事件推送）。

## 3. 运维视角：重置密码怎么做

**密码重置永远是"写入新值"，不是"找回旧值"**。MD5 也是单向的——所谓"能从库里看密码"只在
明文行成立，而那正是要消灭的漏洞（库一泄全完）。

| 场景 | 做法 | 效果 |
|------|------|------|
| 用户忘密码 | `hash_tool <新密码>` 得 bcrypt，`UPDATE users SET password=...` | 立即生效（缓存 ≤5 分钟） |
| 图省事 | 直接写 MD5 甚至明文 | **也合法**：用户下次登录自动升级为 bcrypt |
| 批量导入 | 任意格式 | 同上，随登录逐账号收敛 |

也就是说运维习惯零改变，存储格式自动收敛。`register-service` 已固定产出 bcrypt。

## 4. 回归测试

| 测试 | 守什么 |
|------|--------|
| `bcrypt_test` | 三种格式的判定与校验口径（含 32 位 hex 边界、大写 hex 按明文） |
| `sql_queries_test` | 回写 SQL 必须带 `AND password = ?` CAS 守卫、恰好 3 个占位符 |
| `password_rehash_test`（新增 target） | helper 全链路（mock DB 延迟模式）：明文→CAS 参数→缓存刷新→新 hash 可验；bcrypt 行零 DB 操作；UPDATE 失败缓存不动、done 一次；无效连接/空缓存不崩 |
| `smtps_fsm_concurrency_test::auth_user_db_async` | SMTP 集成路径：明文行 SELECT → CAS UPDATE → last_login → 235 的顺序与参数，缓存已升级 |
| `smtps_fsm_concurrency_test::auth_user_db_async_bcrypt_no_rehash` | 对照：bcrypt 行唯一一次 execute 直接是 last_login，缓存原样 |

双向验证：改动前的 `auth_user_db_async` 只 fire 一次 execute，对新链路直接 FAIL
（第二次 execute 无人触发，235 永不到达）——证明测试对链路形状敏感；改成断言两步后 PASS。

## 5. 相关

- [bugfixes/2026-09-28-auth-cache-ttl-and-password-false-alarm.md](../../bugfixes/2026-09-28-auth-cache-ttl-and-password-false-alarm.md) —— AuthCache TTL 语义（本文的缓存窗口前提）
- `include/mail_system/back/common/bcrypt.h` / `password_rehash.h` —— 口径与契约的权威来源
