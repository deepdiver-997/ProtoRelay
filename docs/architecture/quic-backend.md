# QUIC 传输后端设计决策（2026-10-24）

> Phase A 契约（`include/framework/net/quic_transport.h`）评审通过后的实现决策记录。
> 模块：`protorelay_quic`（可选，第三方库存在才编译）。参考实现：一套经生产验证的
> lsquic 反代工程（同源 vendored lsquic + BoringSSL；本地路径不入库）。

## 线程模型：为什么骨架阶段是一个 NativeLoop 全包

lsquic 的 `lsquic_engine_t` **不是线程安全的**——这是所有线程决策的硬约束：
谁拥有 engine，谁才能调它的 API。收包（recvfrom）、路由（DCID 解析 + 表查）、
发包（`sendmsg`，单数据报内核原子）可以分属不同线程，唯独 engine 处理不行。

由此三条候选形态的取舍：

| 形态 | 判定 |
|---|---|
| 多线程收包 + 内联执行别人的 engine | 不可行：数据竞争 |
| 单线程收包 + 线程池跑 engine | 正确性靠 per-engine strand/锁，丢批处理时机、tick 精度（多一跳 post）、缓存亲和 |
| ingress 与 engine 同一专线程（骨架采用） | 热路径零锁、tick 直接挂 timer、关停次序可控；即参考项目 demux+worker Model B 的退化单分片形 |

**Phase B 实现**：一个 NativeLoop 承载 UDP fd（`add_fd` 可读收包）+ NativeTimer
（`lsquic_engine_earliest_adv_tick` 到期触发 `lsquic_engine_process_conns`）+
流回调。契约中的 executor 就是这个 loop，引擎内部管线是模块黑盒，契约不受影响。

## 将来拆分时的接缝设计（现在不做，只留结构）

多核扩展路径：ingress 线程收包 → 按 DCID 路由 → 投递到各 engine 分片的串行上下文
（每 engine 一线程，或 SO_REUSEPORT 内核分流后每线程一 engine）。决策：

- **投递缝抽象成批队列接口**（`push_batch` / `drain`），骨架实现是"同线程直调"
  这个退化形态；换实现不动逻辑。
- **队列存数据（定长槽拷贝），不存指针**。1500 字节 memcpy ≈ 百纳秒级，指针方案
  省下拷贝却引入堆分配/回收池/cache miss；零拷贝只在 AF_XDP / io_uring recv zc
  前提下才成立。
- **锁策略阶梯**：先互斥 + 小批队（无竞争 ≈ 20–40ns，第三阶开销），profile 指到
  再换 Vyukov/Lapinskiy 式 SPSC 环（约 40 行）。N ingress × M engine 时用 N×M 条
  SPSC 环按 `(producer, engine)` 编号，每环仍免锁。
- **第一阶开销是 syscall 数**：`recvmmsg` 批收、`sendmmsg` + `UDP_SEGMENT` GSO
  批发，比队列策略重要一个量级。
- **唤醒**：生产者写完环 kick 消费者（eventfd/pipe 注册到 engine 的 loop），
  一轮收包只 kick 一次。
- **单 ingress 线程的容量判断**：单核 recvfrom ≈ 0.5–2 Mpps；engine 每包还要
  解密/ACK/拥塞控制，业务不平凡时 engine 先于 ingress 成为瓶颈。邮件系统的
  节点链路流量远达不到"一个收包线程不够"的量级——需要扩时优先加 engine 分片，
  而不是先加收包线程。

## 收发方向

- **收**：统一 ingress（骨架里与 engine 同 loop），按 DCID 分流（非四元组——
  NAT rebinding 时连接不丢；连接迁移是非目标）。
- **发**：engine 回调 `ea_packets_out` 里**直接同步 sendmsg**，多 engine 可共享
  fd（数据报级原子）。"同步"指拷入内核发送缓冲；非阻塞 fd 上缓冲满 → EAGAIN →
  lsquic 保留未发包，fd 可写后 `lsquic_engine_send_unsent_packets()`
  （参考项目 demux 的 write-watch 同款）。异步发送队列没有收益，批量化走 GSO。

## lsquic 补丁取舍（基线 f906160，含上游 PR #690）

参考项目带 A+B 两个补丁（`lsquic-http3-interop-a-b.patch`）：

- **Patch A（无 SNI 回退默认证书）**：只作用于 `LSENG_HTTP` 模式的
  `iquic_lookup_cert`；本模块用裸 QUIC 流（自有 ALPN，非 H3），上游代码在
  非 HTTP 模式本来就走"无 SNI → 默认上下文"路径。**保留在 vendored 源里
  （与参考项目字节同源），对本模块是惰性代码**；将来若接 H3 直接受益。
- **Patch B（RFC 9000 §14.1 Initial 填包重启用）**：上游 4.7.0/4.9.4 的真实
  违规——服务端 Initial 不填到 1200 字节，严格对端（ngtcp2 系 curl 等）直接
  丢弃导致握手挂死。lsquic 自家客户端宽松所以自测不出来。**必须保留**。

## 符号前缀（与 Asio OpenSSL 共存）

参考项目进程里没有第二套 libssl，所以直接静态链 BoringSSL 即可；本仓库
`protorelay_native_tls` 链系统 OpenSSL，同进程共存必须走 gRPC 同款
`BORINGSSL_PREFIX` 全量符号重命名：

- BoringSSL 配置 `-DBORINGSSL_PREFIX=pr_bssl_`；lsquic 与本模块编译带同样定义。
  该 BoringSSL 版本把前缀头（`include/openssl/prefix_symbols.h`，前缀无关、
  编译期由 `BORINGSSL_PREFIX` 宏展开）直接 track 在上游树里，**生成器与 Go
  都不需要**。
- 绝不用 quictls 方案（仍是另一版 OpenSSL，双 libssl 撞符号）。

## 版本钉

- lsquic：`f906160`（"Make the CRYPTO-frame stash limit configurable"，v4.9.4 后）
- BoringSSL：`956bac6e`（"add CBS_get_u48"）
- 同步方式：按 `third_party/quic/MANIFEST.md` 的钉定 SHA 手动拉取并打
  `patches/` 补丁；源码与构建产物只落在各检出的 `third_party/quic/` 下，不入库。

## lsquic 接入实现教训（Phase B/C 调试实录）

Phase C loopback 首跑连挂后逐层定位出的四条库行为，重实现时必须遵守：

- **服务端交付时机 = `on_new_conn`，不是 `on_hsk_done`。** lsquic 的 IETF 服务端
  首飞由内部 mini-conn 处理，晋升为 full conn 时才触发 `on_new_conn`，且晋升路径
  **不会补发 `on_hsk_done`**——等它会导致连接永远不交付。参考反代项目正是在
  `on_new_conn` 交付 session。（客户端仍以 `on_hsk_done` 为准。）
- **`lsquic_stream_writev` 只把数据写进流的缓冲，不进组包队列**——写完必须
  `lsquic_stream_flush`，否则数据静默滞留。同时全量接收后要
  `lsquic_stream_wantwrite(0)`：挂着 1 会让 lsquic 反复回调 `on_write`，
  千次无进展后触发库内死循环保护把流拉黑（write-low）。
- **`wantread` 必须在流生命周期内常驻挂 1。** 读完后清 0，后续到达的数据会被
  库判为"应用不读了"，直接对端发 STOP_SENDING 掐流。`async_read` 到来时立即
  `readv`（数据已在库内缓冲），完成照旧 post，语义不变。
- **libc++ 陷阱：move-out 小 `std::function` 成员后源对象可能仍非空**
  （标准允许 valid-but-unspecified），`if (handler_)` 存活判断会被骗，
  handler 被二次调用、exactly-once 破坏。模块内所有 handler 成员 move 出去后
  一律显式 `= nullptr`；包装 `NativeLoop::invoke_when_live` 的
  `run_on_loop` 也因此按左值传入（内部拷贝进任务，inline 回退不再跑在
  moved-from 捕获上）。

另：BoringSSL 自 `956bac6e` 起移除 `X509V3_EXT_conf_nid`，测试内自签证书用
`X509V3_EXT_nconf_nid(nullptr, nullptr, ...)` 等价替代。
