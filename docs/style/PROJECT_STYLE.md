# ProtoRelay Project Style Guide

This document captures conventions inspired by large production-grade CLI/network projects.

## 1. Naming and Versioning

- Product name: ProtoRelay.
- Versioning: Semantic Versioning (MAJOR.MINOR.PATCH), e.g. 0.1.0.
- Build metadata should be injected at configure/build time (version, commit, target, compiler).

## 2. CLI Contract

- Support `--help` and `--version` as stable interfaces.
- Keep help output deterministic and friendly for copy/paste.
- Exit code conventions:
  - `0`: success/help/version output.
  - `2`: invalid CLI arguments.
  - Non-zero others: runtime/startup failures.
- Unknown options must fail fast with a clear error message.

## 3. Startup Output

- Keep startup banner concise.
- Version detail should come from `--version`, not verbose default startup logs.
- Runtime logs should be structured and module-tagged.

## 4. Compatibility Strategy

- Keep backward compatibility for one positional `config_path` argument.
- New options should be additive and avoid breaking scripts.

## 5. Documentation Discipline

- README should explicitly state:
  - Current implemented scope.
  - Non-goals / not-yet-implemented parts.
  - Extensibility points.
- New user-facing options must be documented in README and `--help`.

## 6. Extensibility Architecture

- Use interface-driven modules for external systems:
  - Database pools.
  - Storage providers.
  - Outbound delivery and DNS routing.
- New providers should integrate via factory/config without touching FSM core logic.

## 7. Build and Reproducibility

- All generated files belong to the build directory.
- Source tree should only keep scripts and templates.
- Build script should auto-heal stale CMake cache/source mismatch.

## 8. Logging and Observability

- Default release log level: info.
- Keep debug-level logs behind compile-time switches.
- Include request identifiers (message ID/mail ID) where possible.

## 9. Security Baseline

- Do not commit secrets (DB password/private keys).
- Prefer mounted runtime secrets/config for deployment.
- Enforce cert/key file existence checks during startup.
- Project must include a `LICENSE` file with the chosen open-source license.

## 10. Test and Change Quality

- For new features, include at least one runtime verification command.
- Keep changes small and focused; avoid unrelated formatting-only edits.

### 10.1 ⚠️ 每个 bug 修复必须配一个专项场景回归测试（2026-09-27 起强制）

**没有回归测试的 bugfix 不允许单独合入**——修复提交与回归测试同一提交推送。
这不是形式主义：DKIM 验签链四个 bug 全部漏网，就是因为旧测试只断言
「wire 里有 DKIM-Signature 头」这种存在性检查（详见
`docs/bugfixes/2026-09-25-dkim-verification-chain-four-bugs.md` 教训 §1）。

写回归测试的四条硬要求：

1. **双向验证（防假绿）**：提交前把修复 stash/revert 掉重跑测试，必须 FAIL；
   恢复修复后必须 PASS。「测过通过」不等于「测得住回归」——只验证过后半段的测试
   可能在修复前就一直是绿的（3.9 之前的 DKIM 测试正是如此）。
2. **验证者/消费者视角**：测「下游实际能验过/能收到」，不测「我们产出了某个字节」。
   例：DKIM 要按 RFC 6376 规范化算法独立重建签名输入并做 RSA 验签
   （outbound_utils_test 3.9），而不是检查头存在；STARTTLS 要断言 handoff 后会话
   拿到的 ListenerConfig（starttls_handoff_test），而不是断言代码跑到了某行。
3. **场景 = 触发条件的最小复刻**：bug 怎么触发的，测试就怎么搭。跨模块时序类
   （pause+worker+drain 级联）在 SessionBase 层用 mock 驱动同样的调用序列
   （pipeline_deferred_test）；字节级类（去点填充跨块）在纯函数层喂分块数据
   （line_folder_test 9）。优先纯函数/mock 层——能在单测里复现的 bug 才守得住。
4. **测试文件头注释写清前科**：事故日期、现象一句话、指向 bugfix 文档。
   让下一个改这段代码的人知道这个测试在守什么。

### 10.2 静默失败路径必须可观测

新增/修改异步链时，每一条「失败返回」路径都要有日志（含关键上下文：
域名/连接数/错误原因）。2026-09-25 入站 DKIM 的 `d2i_PUBKEY` 失败、pubkey-empty
分支全静默，排查只能黑盒探针+二分，代价是整整一轮部署。返回 false 前不留痕
= 给下一次事故埋雷。

### 10.3 线上验证不能只看己方 250

「对端 MX 收下了」≠「对端用户能看到」。投递链路改动后的验收顺序：
mail-tester 报告（外部评分+验签明细）→ 对端真实账号确认 → 己方日志。
DKIM/SPF/DMARC 的每一次改动都走一遍完整验收，不要用「代码看起来对」替代。

## 11. Async / Threading Model

### 11.1 改写 io 对象必须 post 到 io_context

- **事件循环（`io_context::run()` / `post()` / `dispatch()`）线程安全；单个 I/O 对象
  （`steady_timer` / `socket` / `ssl::stream`）不线程安全。** 同一个对象不能在多线程
  并发调用非 const 方法（`expires_after` / `cancel` / `async_read_some` / `async_write`）。
- 常见的误解："`async_*` 会 rebind 回 io_context，所以跨线程安全"。这只保证**完成回调
  在 io_context 线程执行**，并不保证从别的线程改这个对象安全——对象内部状态（如 timer
  接入的 io_context 共享 timer 堆）在 io 线程 dispatch 时会并发触碰，是真实竞争、可致 UB。
- **规则：凡可能被 io_context 所在线程触碰的 io 对象，要改它就 `boost::asio::post(exec,
  lambda)` marshal 到 io_context 任务队列串行执行**——多一次 post-fetch 任务的开销，
  换来与 io 线程 dispatch 之间的串行化。
  - 例：`SessionBase::rearm()` / `disarm_timeout()` 内部 `post` 到 `connection_->get_executor()`
    再改 timer（异步超时回收 watchdog）。
  - 例：远程存储装饰器把阻塞/耗时 op `post` 到 worker 池避免卡 io；回调再续。

新迁移的框架业务代码使用 `pr::net::IExecutor::post` 和 `ITimer`，Asio 类型留在适配器中。
服务器监听和 STARTTLS 使用 `IServerTransportProvider` / `IServerTransport`；
协议层不暴露 context、work guard、acceptor、SSL context 或原始 socket 交接。
`TcpConnection` / `SslConnection` 是持有 `IConnection` 的门面，保持现有会话工厂兼容。
`server_transport_test` 用无 Boost 的替代后端验证接入策略、TLS 交接与 Metrics HTTP；
`asio_server_transport_test` 验证真实监听停止等待回调退出、工厂异常隔离及升级后 executor 不变。
`PROTORELAY_BUILD_BACKEND_CONTRACTS_ONLY=ON` 单独构建这些框架边界中的五项无驱动契约，
不查找 Asio/Boost 或 OpenSSL；生产出站/DB 等尚未迁移的部分仍可依赖 Asio。
服务器维护任务通过 `IExecutorProvider::next_executor()` 取得执行器，工作线程池不承担该职责。
POP3 清扫/心跳的专项回归见 `test/unit/pop3_timer_boundary_test.cpp`（2026-10-19）：
覆盖 worker/DB 回调后的定时器重排、关闭与到期成功回调竞态、排队初始化及析构取消，
以及 POP3 在握手完成前不得发 banner、握手失败必须关闭且不进入命令读取。
POP3 CAPA/LIST/UIDL/RETR 的完整多行响应只发起一次写入；逐行并发写会在 TLS 下
重叠异步操作，并让多个完成回调重复恢复读。延迟写测试在第二次并发写时直接断言。
HTTP/HTTP2 入口同样先完成连接握手再进入读取；HTTP 的握手成功/失败门控见
`test/unit/http_session_read_test.cpp`。

### 11.2 两条都成立的正规异步续跑路径（别误判违规）

1. **回调 `post` 回 io 线程**再碰 io 对象（即 11.1 的规则）。
2. **回调在 worker 线程续跑**，靠调用方先 `set_paused(true)` 取得 session 独占
   （SPF/DNS/commit 回调约定；S3 装饰器即此类）。

选哪条：续跑里**要立刻改 io 对象**就 post 回 io；只要续跑解析内存状态、不改 io 对象，
可在 worker 线程续。

> 心理学捷径：读路径用 `set_paused` 让 worker 与 io 互斥，timer 这类没有 pause 可借的
> 对象用 `post` 把操作挪到同线程——两条都是正确隔离，只是手段不同。

### 11.3 ⚠️ 实参求值顺序 × move 进捕获 —— 本仓库最高频的段错误来源（已踩 4 次）

**同一函数调用里，禁止既解引用/使用一个对象，又把它 `std::move` 进同调用的 lambda
捕获。** 实参求值顺序不确定（不同编译器/优化等级/内联决策都不同），完全可能先执行
捕获的 move，另一实参拿到的是已置空的对象 → 段错误（si_addr 常是很小的字段偏移，
如 0x8）或静默功能失效。

```cpp
// ✗ 禁止
async_connect(*sock, endpoints, [sock = std::move(sock)](...){...});
// ✓ 正确：裸指针先行，再 move
auto* sock_raw = sock.get();
async_connect(*sock_raw, endpoints, [sock = std::move(sock)](...){...});
// ✓ 或者：shared_ptr 拷贝捕获
```

**四次前科**（详见 `docs/bugfixes/2026-09-11-argument-eval-order-and-capture-move.md`）：
96dbae6 accept_connection 段错误、5085ddb IMAP CPS 参数被回调 move 掉（5 处功能失效）、
e435dd5 捕获 timer 而非 session（跨线程 UAF）、2026-09-11 outbound connect_to_mx
（RackNerd 部署必现，排障花了一整轮才定位）。

**必须内化的三句话**：
1. "在别的构建/别的机器上能跑"不是证据——求值顺序是编译器属性，换工具链必翻车。
2. 写最小复现必须**复刻写法模式**（含 move 进捕获），只复刻 API 组合会得出错误结论。
3. 崩溃帧在 asio/库内部 ≠ 库的锅；si_addr 是小常量偏移 = "空对象+字段偏移"指纹，
   先查调用点的捕获/生命周期，再怀疑库。

### 11.4 ⚠️ std::move 后的结构体再次 move/set —— "moved-from 复用"静默抹空

**`std::move(x)` 之后，`x` 成了 moved-from 状态（string/容器 member 为空、值为初值）。若同一作用域里
再次把 `x`（或其成员）`std::move` / 赋值进目标，第二次拿到的是空值——不报错、静默覆盖目标。**

```cpp
// ✗ 禁止：同一 ob_cfg 连续两次 set_config(std::move(ob_cfg))
ob_cfg.default_route = cfg->outbound_default_route;
set_config(std::move(ob_cfg));   // 第一次: default_route 正确装入 config
set_config(std::move(ob_cfg));   // 第二次: ob_cfg 已 move, default_route.host/static_routes 全空 → 覆盖 config_, 路由被抹空
```
**前科**：2026-09-11 `smtps_server.cpp` 组装 `OutboundConfig` 后误写两行 `set_config(std::move(ob_cfg))`，
第二个 move 抹空了 `default_route`/`static_routes` → 运行期 resolve 退回裸域 A（叠加 11.3 的 MX 缺陷），
阿里云 default_route 配了却派发给 `qq.com` 而非 RackNerd。修复：删重复行（a11f7f4）。

**判断口诀**：同个对象的 `std::move` 出现 **≥2 次** = 红警。move 只应发生一次，之后对象视为"已移交，
仅供析构"。若确实要复用同份值，用 **copy**（`= cfg->outbound_default_route`，或保留一份副本）。

### 11.5 ⚠️ io_context::run() 不装 work_guard —— 发起队列静默饿死

**`io_context::run()` 在没有未完成任务时立即返回。** 若给 io_context 起了线程跑 `run()`
但没装 work_guard,线程启动瞬间就退出;之后一切 `post` 到该 context 的任务只入队、
永不执行——无错误、无日志,表现为"响应永远上不了 wire / 发起调用全部失效"。

```cpp
// ✗ 禁止:run() 无任务立即返回,线程空转退出,后续 post 全部饿死
exec_thread_ = std::thread([this]() { exec_ctx_.run(); });

// ✓ 正确:guard 持活,stop 时 reset → stop → join → restart
exec_thread_ = std::thread([ctx = exec_ctx_]() {
    auto guard = boost::asio::make_work_guard(*ctx);   // guard 放线程栈
    ctx->run();
});
// stop: guard_.reset() 或 ctx->stop() → join → ctx->restart()
```

**前科**：2026-09-16 `mock_connection.h::start_executor()`——CI 的 unit-test/coverage/
asan-unit 三 job 连红 18 天的最后一块拼图。09-06 起 `SessionBase::do_async_write/close`
的发起都 post 到连接 executor(mock 里=真实 io_context),fixture 没跑它 → greeting/
CAPABILITY 等首响应全部发不出来,4 个 FSM 测试全挂在第一个断言。

**判断口诀**：凡是"post 进去了但回调永远不执行、且无任何报错",第一反应查跑该
context 的线程是否还活着——run() 没装 guard = 线程早已退出。guard 放线程栈上
（make_work_guard 后立刻 run）,stop 顺序 = reset guard → stop → join → restart。

## 12. Error Codes and Exceptions

- `std::error_code` 是错误值，不是异常；异步读写、计时等可预期失败通过完成回调返回。
- 框架自有语义使用自己的枚举与 `std::error_category`（如 `net::IoErrc::end_of_stream`），
  可用标准条件表达的语义优先使用 `std::errc`（如取消、超时）。调用方按完整错误码/条件比较，
  不依赖裸整数：错误身份由 category 与 value 一起决定。
- Asio 适配器在 `net::asio_error()` 统一 EOF/取消，其余驱动错误保留原 category、value 和诊断。
  不把所有驱动值重新装进 `std::system_category()`，以免相同数值被解释成另一种错误。
- 同步驱动系统异常在 `net::asio_call()` 边界转为 `std::system_error`，原始驱动异常通过
  `std::nested_exception` 保留；参数/编程错误及用户异常保留原类型，不能一律伪装成 IO 错误。
  完成回调在后续调度中执行，不能把用户回调抛出的异常错算成驱动发起失败。
- 不为替换标准库载体而新建一套继承树。若未来需要统一重试策略、错误上下文等能力，
  可以增加基于标准错误码的框架分类，协议状态（如 `SessionError`）仍保持自身语义。
- 握手和执行器的回调容器目前是 `std::function`，要求 callable 可拷贝。持有 `shared_ptr`
  的 lambda 可以直接存放；即使 session 由 `shared_ptr` 管理，捕获 `unique_ptr` 的 lambda
  仍然不可拷贝。确需移动型回调时应统一调整回调容器，避免局部偷偷增加共享所有权。

错误域及同步异常转换回归：`test/unit/asio_error_test.cpp`。
