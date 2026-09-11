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
