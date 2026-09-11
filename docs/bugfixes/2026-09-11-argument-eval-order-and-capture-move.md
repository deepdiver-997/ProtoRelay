# 实参求值顺序 × move 进 lambda 捕获 —— 本仓库反复踩坑的一类 bug

- 日期：2026-09-11 系统性成文（此前至少 4 次独立踩坑，见下）
- 类别：C++ 求值顺序 UB / lambda init-capture move / 捕获悬垂
- 严重度：段错误或静默功能失效，**无日志、复现依赖编译器与优化等级**

## 规则（先读这个）

**在同一个函数调用里，不允许既解引用/使用一个对象，又把它（或其成员）
`std::move` 进同调用的 lambda 捕获。** C++ 实参求值顺序不确定（C++17 也只保证
嵌套限定，不保证实参间顺序），编译器完全可能先执行捕获的 move 再求值另一实参。
先取裸指针/拷贝，再 move；或改用 shared_ptr 拷贝捕获。

```cpp
// ✗ 禁止：*sock 与 std::move(sock) 同处一次调用
boost::asio::async_connect(*sock, endpoints, [sock = std::move(sock)](auto...) mutable {...});

// ✓ 正确：裸指针先行
auto* sock_raw = sock.get();
boost::asio::async_connect(*sock_raw, endpoints, [sock = std::move(sock)](auto...) mutable {...});

// ✓ 正确：shared_ptr 拷贝捕获（引用计数保护，回调内自行 move 私有副本）
boost::asio::async_connect(*sock, endpoints, [sock](auto...) mutable {...});
```

同理适用于：`std::move(x)` 进捕获的同时，其他实参里还出现 `x`、`*x`、`x.get()`
的任何形式。

## 本仓库的四次实例

### 1. 96dbae6 — accept_connection 段错误（framework）

`server_base.cpp::accept_connection` 异步接受连接：捕获参数与 `lowest_socket`
的计算混在同一调用，实参求值顺序不同平台/编译器不同 → 引用已失效的 socket。
修法：提前把值算好再进调用。原文明确写了"避免因平台捕获参数和计算传递参数
顺序标准不一可能引发的段错误"。

### 2. 5085ddb — IMAP CPS 参数被回调 move 掉（登录/SELECT/STATUS/APPEND 全失效）

CPS 改造时把同一值 `std::move` 进异步函数的回调捕获，同时它还是该函数的形参。
求值顺序不利时异步函数拿到的是 move 后的空值 → `User not found` / `Mailbox not
found`。单测 `!w.empty()` 断言抓不住（empty 检查在 move 之后）。修法：回调捕获
改 copy。5 处：handle_login / select / examine / status / append。

### 3. e435dd5 — disarm_timeout 捕获 timer 而非 session（跨线程 UAF）

close() 可能来自析构线程，捕获 session 再 post 回定时器 io 线程 = 依赖
`shared_from_this()`（析构期抛 bad_weak_ptr）+ 跨线程访问 ASIO 对象。修法：捕获
timer 自身 shared_ptr。教训同族：**捕获谁，谁就必须保证在回调执行时仍存活**，
且不得依赖"刚被 move/析构"的对象。

### 4. 2026-09-11 — outbound connect_to_mx 段错误（RackNerd 部署必现，本次）

```cpp
auto sock = std::make_unique<boost::asio::ip::tcp::socket>(io_ctx2);
boost::asio::async_connect(*sock, endpoints,          // ① 解引用
    [self, sock = std::move(sock)](...) mutable {...} // ② move 置空
```
交叉工具链产物实参求值 ②先于 ① → `*sock` 解引用空 unique_ptr → socket 对象
位于 0 地址 → asio `range_connect_op::process` 读 +8 偏移 → SIGSEGV si_addr=0x8。

- **现场特征**（供以后秒判）：`Outbound: connected to ...` 之后无任何日志、
  无超时、无崩溃日志；outbox 行卡 SENDING 且被轮询自动重试 → 反复崩溃重启循环。
  三个 core 全部 si_addr=0x8、同一指令地址。
- **排障弯路**（引以为戒）：30 行最小复现"复现不出"就下结论"工具链问题"是错的——
  复现程序必须包含与线上一字不差的**写法模式**（这里是 move 进捕获），而不是
  只覆盖"同一批 asio API"。最终 gdb 上看 cout 对象是坏指针也一度把方向带偏到
  静态初始化。真正的定位靠：换自洽工具链构建仍崩 → 逐 diff 代码变更 → 重读
  asio process() 源码 + 反汇编对上 +0x8 字段偏移。
- 修复：`sock_raw = sock.get()` 先行（见 outbound_smtp_session.h connect_to_mx）。

## 为什么单测/压测抓不住

- 求值顺序是**编译器+优化等级+内联决策**的属性：同一份代码 macOS 原生/e2e 容器
  可能侥幸不炸，换交叉工具链必炸——"在别的构建下能跑"不是证据。
- 崩溃点在 asio 内部（崩溃帧≠肇事帧），无 -g 时 addr2line 只能到内联大函数，
  符号名具有误导性。
- 静默变体（5085ddb）完全不崩，只是功能坏，日志一行不剩。

## Code Review 清单（asio/CPS 代码必查）

1. 同一调用里出现 `std::move(x)`（尤其 init-capture）时，逐实参检查 `x`/`*x`
   是否还被求值。
2. lambda 捕获 unique_ptr/独占资源时，回调外是否还有指针在用同一对象。
3. 捕获 raw pointer + move 捕获的组合：裸指针只在 async 调用**发起瞬间**解引用，
   生命周期由 move 进捕获的独占指针延续。
4. 怀疑此类崩溃时：core 的 si_addr 是不是很小的常量偏移（0x8/0x10/0x18）——
   这是"空对象+字段偏移"的指纹；换自洽构建重测只是排除变量，不是结论。
