# AuthCache TTL 失效：过期条目永续命中 + "三账号密码失效"假案

日期：2026-09-28　|　影响面：smtpsServer / imapsServer 认证缓存　|　状态：已修复

## 问题一（真 bug）：过期条目被当作有效缓存

`AuthCache::lookup`（auth_cache.h）调 `LruCache::get` 却丢弃 stale 标志——而 `get`
的契约是"命中即返回，stale 标志标注是否过期"（lru_cache.h:69）。结果：条目在被
LRU 挤掉之前一直生效；容量 10000、小用户量下几乎永不淘汰。**DB 改密码/解禁账号
必须重启进程才生效**，且 SMTP/IMAP 是两个进程各一份缓存，要重启俩。

注释与实现不符：auth_cache.h 原注释写着"即使 TTL 过期也会返回"——写注释的人
（2026-09-28 之前的自己）知道这个行为，没意识到它是坑。

### 修复

- `lookup`：stale 视为 miss，调用方走 DB 路径重查并 `store` 回填
  （`LruCache::put` 对已有 key 刷新时间戳，lru_cache.h:79，无钉死风险）。
- 代价：每账号每 TTL（5 分钟）至多一次 DB 查询，可忽略。
- 显式失效钩子已存在（`LruCache::invalidate/invalidate_if`），将来 web 端改密码
  想做即时生效可加内部失效接口，不必为此引入跨进程通知。
- 回归测试 `auth_cache_test`（新增 target）：新鲜命中/miss、过期→miss、
  回填刷新时间戳与值、负缓存（禁用账号）TTL 内快速拒绝。

## 问题二（假案）：文档密码三账号"登录失败"

qt/test/test3 用文档密码登录全部失败、bcrypt 账号正常——排查半天 DB 路径与缓存
路径的明文比较，代码都是对的。真凶是**测试工具自己**：

```
printf 'a1 LOGIN test3@scut.email test123\r\n' | openssl s_client -crlf
```

`-crlf` 会把输入的 LF 再翻译成 CRLF，与 printf 已带的 \r\n 叠加，线上实际发出
`\r\r\n`。服务端剥掉行尾 CRLF 后，最后一个**裸词 token**（密码）粘了一个多余的
`\r` → 比较失败；带引号时解析器从引号内取值不受影响 → "引号版成功、裸词版失败"
的假象。同样的 `\r\r\n` 也解释了当时 `CAPABILITY → BAD Unknown command` 的怪象。

结论：**密码列数据完好、文档密码正确、认证代码无 bug**，无需改密码。
教训：openssl s_client 测行协议时，printf 只给 `\n` 让 `-crlf` 去翻译，
或不用 `-crlf` 且 printf 给全 `\r\n`，二选一。

### 附带确认

- IMAP `AUTHENTICATE PLAIN` 未实现（回 NO Unsupported mechanism），客户端走
  LOGIN（TLS 之上可接受）；要支持可后补，base64 解码路径不受行尾问题影响。
- 深挖时确认 `deepdiver-997` 每次登录 ~270ms 不是缓存未命中，而是缓存命中后
  bcrypt_verify 的固有计算成本；明文账号同毫秒完成 —— 时间线与缓存行为自洽。
