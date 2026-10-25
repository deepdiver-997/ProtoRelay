# 节点消息协议 v1（node proto）

> 模块：`protorelay_node_proto`（`include/framework/node/node_proto.h`、
> `src/framework/node/node_proto.cpp`）。承载层：NodeLink v0 点对点帧
> （`node_link.h`）。本协议定义节点之间端到端的消息信封、受控转发与安全语义。
> 控制面（签名后的 epoch-config、身份与通道协商）归 node-link-v1 会话，本协议不涉及。

## 信封与线格式

信封作为**一个** NodeLink notify 帧的 payload 传输（request/response 不用于消息语义）。
所有整数大端，与帧层同一手工编码风格，畸形即拒：

```
[u8  version = 1]
[u8  type]                  1=data 2=ack 3=error 4=probe
[u8  flags]                 bit0 = payload 为 AEAD 密文；bit1..7 保留，必须为 0
[u8  key_id]                加密时选择预共享端到端密钥
[u32 ttl]                   剩余可转发跳数
[u32 nonce]                 发送方随机数（重放防护）
[u64 timestamp_ms]          发送方时钟，Unix 毫秒
[u64 message_id]            端到端唯一消息 id
[u16 src_len][source]       源 node_id（1..255 字节）
[u16 dst_len][destination]  目的 node_id（1..255 字节）
[u8  route_len]             中继列表长度（不含源与目的）
[u8  route_pos]             已消费的 route 条目数
route_len × [u16 len][hop]  中继 node_id（各 1..255 字节）
[u32 payload_len][payload]  载荷（密文时为 ciphertext||16 字节 tag）
```

约定：

- `Envelope.payload` 在 Router API 中**永远是明文**；codec 自由函数对 payload
  逐字节透传（便于构造/解析密文形态做测试）。
- `send()` 时 ttl/nonce/timestamp/message_id 为零值则由 Router 填充
  （ttl=max_hops，其余随机/当前时钟）；caller 给定值原样上线（测试依赖这一点）。
- 路由三种形态：`route=[]` 纯一跳（dst 必须是邻居）；`route=[b]` 一跳中继；
  `route=[b,c]` 受控多跳（源指定路径）。中继只按 route/dst + 本地邻居表决策，
  不要求全图知识——路由器是哑管道。

## 转发语义

节点收到信封后按 `destination` 分流：

- **dst == self（投递）**：时间戳容差 → nonce 窗口 → message_id 去重（可选）→
  解密（若加密）→ 按类型分发（error → 回调；probe → 自动回 ack，不上抛应用；
  data/ack → `on_message`）。
- **dst != self（转发）**：message_id 环检测缓存（有界 LRU，重复静默丢弃）→
  route 校验（`route[route_pos]` 必须是自己，否则视为偏离路径）→ 解析下一跳
  （中间中继取 `route[route_pos+1]`，最后一个中继取 dst）→ ttl 检查
  （ttl==0 拒绝转发）→ ttl-1、route_pos+1 重编码转发。

ttl 语义：`send()` 写入 `max_hops`；每经过一个转发节点减一；ttl==0 的信封
不能再被转发。因此 ttl 值等于剩余允许的转发次数。

**队列有界**：每个 Router 维护 `max_in_flight`（已交给链路、尚未完成的数据消息数，
默认 128），超限时本地发送以 `no_buffer_space` 完成，转发丢弃并回报
`queue_overflow`。Router 生成的回报与 probe-ack **不占用**该配额（饱和队列必须
仍能回报），其量级由底层 NodeLink 链路队列（`max_queued_frames`）兜底。

## 重放防护

1. **时间戳容差**：`|now - timestamp_ms| > timestamp_tolerance`（默认 30 s）拒绝。
2. **nonce 窗口**：按 source 维护有界 nonce 集合（每源 `nonce_capacity` 条，
   至多 `max_tracked_sources` 个源，保留期 ≥ 2×容差）；窗口内重复的 (source, nonce)
   拒绝。两者命中都回报 `replay_rejected`。
3. **端到端 message_id 去重**由接收端会话决定（`dedup_by_message_id`，默认开）：
   命中**静默丢弃**——它同时承担转发环防护（每个转发节点都查同一缓存），
   上报反而会在环路里放大消息。

## 加密（端到端）

v1 两种模式：明文（flags bit0=0）与**预共享密钥 AEAD**（bit0=1）。密钥协商不在
本会话范围；`add_key(key_id, 32B)` 预注入。

- 算法：AES-256-GCM（OpenSSL EVP），构建无 `OpenSSL::Crypto` 目标时模块编译为
  不带 AEAD：加密发送以 `not_supported` 失败，加密到达报 `decrypt_failed`——均 fail-closed。
- 12 字节 nonce = `message_id(8B BE) || nonce(4B BE)`，每消息唯一。
- AAD = 除 payload 外的全部头部，**其中 ttl 与 route_pos 置零参与计算**——这两个
  字段逐跳改写，收发双方按同一规则归零后得到一致 AAD。
- 中间节点无密钥、无解密路径；转发对密文逐字节透传（仅改 ttl/route_pos）。
- 解密失败（无 key_id / tag 不匹配）丢弃并回报 `decrypt_failed`。

## 错误回报

error 信封：`source`=检出节点，`destination`=原始 source，`route`=已走过路径的
逆序（不含两端），payload = `[u8 code][u64 failed_message_id]`。回报走与数据相同
的转发机制。错误码：`unroutable`（下一跳未知/偏离路径/链路失败）、`ttl_exhausted`、
`replay_rejected`、`decrypt_failed`、`unknown_type`、`queue_overflow`。

抑制规则：**error 信封与 Router 自动生成的 ack 永不再触发回报**（任何拒绝路径上
静默丢弃），杜绝 error-of-error 风暴；本地发送失败不回报给自己（source==self 抑制）。

## 未知版本 / 未知类型 / 畸形（成文规则）

| 情形 | 处理 | 理由 |
| --- | --- | --- |
| 未知 version | **静默丢弃**，不断链、不回报 | 未知版本的布局不可信，无法可靠读出 source；报文层异常不升级为传输层故障（那是 NodeLink 帧校验的职责） |
| 已知 version、未知 type | 丢弃 + 沿反向上报 `unknown_type` | 布局可信，source 可读 |
| 畸形（截断/超长/保留 flag 位/长度字段不一致/越界 route_pos） | **静默丢弃**，不断链 | 任何字段（含 source）都不可信 |

三种情况都不关闭底层 NodeLink——链路健康是传输层的事；后续合法消息正常收发。

## 版本演进

- 新增消息类型：在 `MessageType` 注册新枚举值即可，旧节点按"未知 type"规则回报，
  不影响转发。
- 布局变更：升 version。旧节点静默丢弃未知 version；新节点对旧版本按兼容策略
  处理（v2 立法时明确）。
- 新能力位走 version 升级而非保留 flag 位（保留位非零直接判畸形）。

## 非目标（v1）

节点发现、拓扑动态维护、QUIC、cover traffic、控制面消息、业务（邮件/传输任务）接入。
测试覆盖见 `test/unit/node_proto_test.cpp`（编解码防御、一跳/受控多跳、环检测、
TTL、重放、加密不可读、未知版本/类型、队列有界、停机生命周期，全部跑双 poller 后端）。
