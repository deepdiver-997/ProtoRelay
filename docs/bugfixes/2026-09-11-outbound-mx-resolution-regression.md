# 出站 DNS 直投从不做 MX 解析 —— 死代码回归 (2026-09-11)

- 日期：2026-09-11
- 类别：出站投递路由 / 死代码回归 / 多机制漂移
- 严重度：**外投到独立 MX 子域的域名(QQ/Gmail 等)全部失败，跨 RackNerd 全链路验收暴露**

## 现象

用户 → 阿里云 587 AUTH → 阿里云 outbound → RackNerd:2525 信任中继 → RackNerd 直投 QQ。
RackNerd 侧一直在 `Outbound: connect to qq.com failed, retry 1/3`；手动 python 投
`mx3.qq.com` 却一次成功（220 + `250 OK: queued as.`）。QQ 未收到，引擎永远连不上。

## 根因

`include/.../session/outbound_smtp_session.h` 的 `connect_to_mx()`：

```cpp
resolver->async_resolve(mx_host_, ...)   // mx_host_ = 裸域名 "qq.com"
```

对 DNS 直投目标（无 static/default 路由），dispatch 的 `resolve_target_host()` 返回**裸域名**，
`connect_to_mx` 再用 boost `tcp::resolver` 做 **A 记录**解析 →
拿到的 `qq.com` 的是 **Web 服务器 IP**（不是邮件服务器）。QQ/Gmail 等 MX 指向独立子域
(`mx1/2/3.qq.com`) → 永远连到非 SMTP 地址。

而**正确的 MX 解析** `build_target_hosts()`（`mx_routing_utils.cpp`：cares 查 MX → 每 MX 主机
A → IP 列表）是**死代码——全仓库零调用**。两套路由机制并存，活跃路径丢掉了 MX 解析——重构回归。

追因线索：出站派发曾用 `build_target_hosts`，后改用更薄的 `resolve_target_host`，后者只做
static/default 路由判断，未保留 MX 解析；`build_target_hosts` 被孤立。

## 修复（commits 58dc3cd + 8f937e3）

1. `outbound_server.cpp` 新增 `resolve_mx_pool(domain)`：`SyncDnsWrapper`(cares, worker 线程可行)
   解析 MX→A 得 IP 列表（含 RFC5321 implicit-A 兜底）；dispatch(submit + on_claim_complete)
   对 DNS 直投目标（`target_host == domain`）注入会话 `set_mx_pool`。
2. `outbound_smtp_session.h`：`mx_pool_` + `pick_connect_host()` 逐个取 IP；
   `handle_connect_failure_or_next()` 失败换下一 IP（IO post 链式），池耗尽落
   `handle_connect_failure()`（退避重试 idx 重置）；每 IP 一个 `CONNECT_TIMEOUT_SEC` 超时
   （不改走整个 `async_connect` 范围，避免一次超时 cancel 掉全部端点尝试）。
3. 静态/默认路由命中则池空，直接连既定 host —— 阿里云→RackRerd(default_route)不受影响。

## 复盘要点（为什么单测/此前压测没抓住）

- `build_target_hosts` 有正确实现但从未被接线，且**没有调用点 = 没有测试覆盖**；
  旧逻辑(A 裸域)在 e2e 里对"A→B(同机 .local 短路)"这种 MX==A 的场景碰巧能跑，
  掩盖了真实公网域名(MX≠A)的缺陷。
- 手动 `nc python 直连 mx3.qq.com` 是验证"IP 可达 + 对方收不收"的**传输层/对方**问题，
  不是验证"引擎的路由/解析"——测错了对象，得出"网络没问题"的误导性结论。
- 教训：**路由/解析类逻辑必须有单元测试锁死语义**（MX→A→IP、静态优先、implicit-A 兜底），
  不能只靠 e2e 冒烟。已补 `outbound_utils_test Section 4`（MockDnsResolver Sync）。

## Code Review 清单

1. 出站目标解析：DOM（domain）→ 必须 MX 解析；只有 static/default 路由才跳过。
2. 并存两套"路由函数"时，确认活跃路径没把正确的解析者孤立成死代码。
3. `async_connect` 走端点范围 + 整体超时 cancel 会毁掉多端点 failover：
   黑洞端点需 per-端点超时。