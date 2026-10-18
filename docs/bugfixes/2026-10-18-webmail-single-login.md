# 2026-10-18：新 Webmail 登录撤销旧会话

用户明确“防多地登录”指新登录踢掉旧会话，覆盖上一阶段多设备并存的假设。
原 4a183d6 只按 token 插入多行，A、B 两次成功登录后两份 cookie 均可使用。

## 双向验证

先加测试，保持旧实现/旧 schema 不变：

- `webmail_test --single-login-regression` 实测 FAIL，替换后旧 token 仍可 lookup。
- 本机双进程 smoke：A 登录、B 登录后，用 A cookie 请求 `/api/me` 应 401，
  旧二进制实测 200，断言 `new login must revoke old cookie` FAIL。

修复后原断言 PASS。扩展 smoke 在两个节点并发执行 8 个登录，全部成功后检查
所有返回 cookie，只有一份有效、DB 账号行数为 1；旧 cookie 的 me/邮箱/发信均
401，旧设备再 logout 不影响新 cookie。错误密码不踢当前设备，不同账号互不干扰。
节点重启后同一当前 cookie 仍可读信/发信，跨协议配额、别名和改密场景继续通过。

## 实现与失败边界

数据库为 `(account_shard,user_id)` 添加唯一键，单条 INSERT … ON DUPLICATE
KEY UPDATE 轮转 token、版本和到期时间。并发按 DB 写入顺序串行化，不先单独
删除旧记录。logout 仍按 token_hash 删除，避免旧设备的注销误撤新会话。
新登录先验证唯一约束的列和唯一性，缺迁移/错误索引/查询失败均返回 503。
内存测试后端以互斥锁保护账号当前 token 与 LRU 的替换和注销。

迁移先停旧写入节点，按账号保留最新创建的一行（同秒按 token_hash 定序），
删除其余行后添加唯一键。`webmail_single_login_migration_test.py` 只用随机临时
表验证去重、时间相同的排序、相同用户 ID 的分片隔离、唯一键与重复执行，不碰
真实用户或邮件。迁移已在本机 shard 0 应用并重复执行验证。

“立即失效”指新登录写入成功后，旧 cookie 的后续鉴权请求返回 401；已经鉴权
并开始处理的请求不撤回。并发响应可能乱序，以 DB 最后成功写入为准。

```bash
cmake --build build-release -j 8
./build-release/webmail_test --single-login-regression
ctest --test-dir build-release -j 4 --output-on-failure
.venv/bin/python test/scripts/webmail_send_smoke.py --db-config config/db_config.json
.venv/bin/python test/scripts/webmail_single_login_migration_test.py --db-config config/db_config.json
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-asan -R '^webmail_test$' --output-on-failure
```
