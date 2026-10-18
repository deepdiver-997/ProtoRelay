# 2026-10-18：Webmail 跨节点会话回归

旧实现仅把 token、账号和 SMTP 密码放在登录节点的 LRU。负载均衡将下一请求
转到另一节点时，`/api/me` 返回 401；节点重启同样丢会话。单节点内存单测无法
发现这个部署问题。

专项场景加入 `test/scripts/webmail_send_smoke.py`：启动两个独立进程，A 登录，
原 cookie 请求 B 的 `/api/me` 必须 200。旧 `b3f3491` 二进制实测 FAIL：
`('cross-node session must be valid', 401, {'error': 'not authenticated'})`。
当时复用原 SMTP 提交路径，测试未依赖新表或新服务 API，负验证真实执行旧版本。

修复用共享 DB 会话注册表，token 仅存摘要。生产异步鉴权查权威注册表和账号库，
发信改用注入的 MailSubmissionService，消除后续发信仍依赖登录节点 SMTP 密码
的限制。HTTP 响应前不阻塞 IO 等 DB；明文密码不进入会话或数据库。

正验证沿用同一跨节点断言，并增加 B 重启、独立设备登录/注销、跨节点发信/读信、
禁用、到期、改密、共享配额和别名去重。`webmail_test` 覆盖 DB 查询失败为 503、
账号/凭据失效、fresh 登录忽略旧 AuthCache、rehash CAS 被改密抢先时拒绝旧密码。

运行命令与部署边界见 `../mail-system/architecture/webmail-sessions-and-submission.md`。

另做两项双向验证（临时恢复旧语义后仅重建 webmail_test，测试源码保持不变）：

- 恢复 AuthCache 无条件命中，`webmail_test --auth-cache-regression` 实测 FAIL，
  旧密码被接受，`!r.ok` 断言失败；恢复 fresh 门禁后 PASS。
- 恢复 DB 回调内联验密，`webmail_test --auth-worker-regression` 实测 FAIL，
  回调在派发到 worker 前已执行；恢复 worker continuation 后 PASS。真实 MariaDB
  非阻塞回调在 IO executor 上，仅把最初登录函数 post 到 worker 不足以隔离 bcrypt。

`webmail_submission_test.cpp` 还用延迟存储验证 commit 完成前不能响应成功、
commit 失败能终结、commit 期间关闭队列时清理正文/sidecar；内存池驱动不依赖睡眠。
