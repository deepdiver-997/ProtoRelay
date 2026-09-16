# CI 双红：asan-unit 全 indirect 泄漏 + coverage 负计数（顺带挖出被掩盖的 UAF 与 lcov 报错）

- 日期：2026-09-16 定位并修复
- 影响：CI 的 `asan-unit` 与 `coverage` 两个 job 连续 8 次 push 全红
  （自 2026-08-28 起；`unit-test` / `full-build` / `fuzz-smoke` 一直绿，
  于是"红了 8 次"被当成常态噪声，没人在意）
- 生产：无影响。四条都是**测试基础设施**问题；但其中第 2 条（UAF）是真实内存
  错误，只是在单测里发作

排查过程中共定位 4 条独立故障，其中 2 条被更早的故障**掩盖**（前面的错断开后
后面的才露出来）：

| # | 故障 | 表现 | 归属 |
|---|---|---|---|
| 1 | 测试会话收尾缺失 → 对象图成环 | asan-unit 5 个用例 LSan indirect leak | 测试基础设施 |
| 2 | 会话活过 fixture 的 server → heap-use-after-free | coverage 构建 ctest SEGFAULT | 真实内存错误（测试侧触发） |
| 3 | gcov 计数器非原子 + 头文件跨 TU 合并 → 负计数 | coverage 在 lcov capture 阶段中断 | 构建/工具链 |
| 4 | lcov 2.0 把"未命中的 exclude 模式"当错误 | coverage 在 lcov remove 阶段中断（被 #3 掩盖） | 工具链 |

## 现象（CI 观测）

**asan-unit**：5 个测试失败，测试逻辑本身全过（如 `pop3_fsm_test` 输出
`33 passed, 0 failed`），挂在进程退出时的 LeakSanitizer：

| 测试 | 泄漏量 |
|---|---|
| pop3_fsm_test | 563,675 B / 769 处 |
| session_base_test | 54,864 B / 33 处 |
| imaps_fsm_test | 26,583 B / 132 处 |
| fsm_base_test / fast_fsm_base_test | 各 18,288 B / 11 处 |

报告里**全是 indirect leak，零 direct leak**。

**coverage**：`ctest` 23/23 全过，之后 lcov 抓取阶段直接报错退出：

```
geninfo: ERROR: Unexpected negative count '-1' for /usr/include/c++/13/bits/fs_path.h:1315.
	Perhaps you need to compile with '-fprofile-update=atomic
```

## 排查

### #1 零 direct leak 是"纯引用环"的指纹

LSan 把只能从其他泄漏对象到达的对象标为 indirect。全是 indirect、一个 direct
都没有，说明不存在"忘了 delete 的裸对象"，而是**一整棵互相持有、与根不可达的
对象图**。从堆栈读出环的形状：

```
session ──unique_ptr──> connection_ ──shared_ptr──> exec_ctx_（mock 的真实 io_context）
   ^                                                      │
   └────────────── 队列里的发起 lambda（捕获 self）────────┘
```

`SessionBase` 09-06 起把所有发起（读/写/close/rearm）post 到连接 executor 并
**强捕获 `self`**，这是刻意的生命周期不变量（"会话活到全部在途 op 走完"）。
生产环境有常驻 IO 线程排空队列，环自然解开；单测里没人驱动队列时，这些 lambda
就成了会话的最后一批持有者，环永远不破。

定位到触发条件后规律非常干净——**只有"会话还活着时被 close()"的用例才漏**：

- 从未显式 close 的用例反而干净：`~SessionBase` 里 `shared_from_this()` 抛
  `bad_weak_ptr` → 走同步关闭分支（不 post）→ 直接析构。
- 会话活着时 close（FSM terminal 分支 / `handle_error` / 用例显式 close）：close
  的发起 post 持 self 进队列，用例一结束就没人排空 → 成环。
  `session_base_test` 里 15 个块 close 后都有 `pump_executor()`，恰好漏了 3 个块。

IMAP 侧还有第二类强引用：**watchdog 定时器的 `async_wait` handler 强持 self**，
只有 `close()` 会 disarm。两个不走 Handle 的手工 session 用例
（`fetch_full_path_with_storage` / `fetch_many_mails_no_stack_overflow`）漏的正是
它们俩，且连带把局部 `fsm2` 的转移表一起钉漏。

pop3 更隐蔽：`~Handle()` 早就 close 了会话，但 **close 自己的 post 仍留在队列里**；
而且它是**偶发**的——容器里 6 次跑 2 次红，每次泄漏字节数完全相同。原因是 close
常由 worker 线程的异步续作触发（DB 查询回调、POP3 心跳续约失败回调），那些续作
的后续 post 会晚于单次排空几十微秒到达，单次 `pump_executor()` 必然漏。

### #2 被 SEGFAULT 掩盖的 heap-use-after-free

修完 #1 再跑 **coverage 构建**时，`smtps_fsm_concurrency_test` 开始随机
`SIGSEGV`，报错还很有误导性：

```
All concurrency tests passed.          ← 用例全过，崩在退出阶段
libgcov profiling error: ...gcda:Merge mismatch for function 7039
libgcov profiling error: ...gcda:Error writing
Segmentation fault
```

看着像 libgcov 的 bug（我也先按这个方向查了：平台默认值、gcda 合并路径、重复运行），
但同一份代码在**无插桩构建**里 5/5 干净 → 说明是插桩改变了时序、把某个潜在竞态
推到了台面上。用 `-fsanitize=address` + `--coverage` 组合重建该 target，ASan 直接
给出真凶：

```
ERROR: AddressSanitizer: heap-use-after-free
  #2 ServerBase::push_metric_observe            src/framework/server_base.cpp:291
  #3 SessionBase<MockConnection>::close()       include/framework/session_base.tpp:43
  #4 SessionBase<MockConnection>::handle_error  include/framework/session_base.tpp:162
  #5 do_async_read 的读完成回调                  include/framework/session_base.tpp:194
  ...
  #15 MockIoContext::run_loop()                 test/unit/mock_connection.h:97   ← 后台线程
```

即：**会话活过了它的 server**。在途读完成的回调持着 session 存活，`SessionHandle`
一直没复位，于是 session 一路活到 `main` 结束——那时 fixture 的 `server` 已经析构，
回调里的 `close()` 去摸 `m_server->push_metric_observe(...)` 就是 UAF。
无 ASan 时这次野写砸在 libgcov 的计数/写盘缓冲上，才呈现出"libgcov 报错 + SEGV"
的假象。

### #3 负计数：计数器丢更新 + 头文件跨 TU 合并

负计数不是覆盖率数据，而是**计数不自洽**的产物：geninfo 对非分支行按"父块计数 −
各出边计数"反算 arc 计数，父块计数被并发读改写丢掉更新后，反算结果就成了 `-1`。
gcov 计数器默认是普通读改写（`-fprofile-update` 默认 `single`，实测 `gcc -Q
--help=common` 与对象文件记录的命令行都是 `single`），而单测里有线程池、mock
executor、并发用例，压力足够就丢更新。

报错行 `/usr/include/c++/13/bits/fs_path.h:1315` 还提示第二种成因：**libstdc++
头文件被多个 TU 分别插桩**，geninfo 合并各 TU 的同名内联函数计数时也会产出负值伪影。
两种成因都不是"真实覆盖率信号"，这正是 lcov 自己的提示里同时给出"改用 atomic"
和"用 `--ignore-errors negative` 绕过"两条路的原因——两条路我都要了。

需要说明的是：**本地（arm64）复现不出这个负计数**（写了 8 线程热循环微基准，
计数器用的还是默认 `single`，仍不复现），它依赖真实负载/时序；CI 的 x86_64 首次
全量跑 23 个测试时中了招。所以这条修复没法做本地 A/B，只能靠"根因治理 + 兜底"
两道防线覆盖两种成因。

### #4 被 #3 掩盖的 lcov 报错

把 capture 修通之后，脚本在下一步又断了：

```
lcov: ERROR: 'exclude' pattern '*/generated/*' is unused.
	(use "lcov --ignore-errors unused ..." to bypass this error)
```

lcov 2.0 把"某个 exclude 模式没匹配到任何文件"当**错误**直接中断。本环境里
`*/generated/*`、`*/.venv/*`、`*/build-cov/*` 三个模式都没命中（没有 .venv、
`build_info.h` 不产生插桩数据）。CI 之前停在更早的 capture 阶段，所以从没走到这里。

## 修复

### 1 + 2. 测试收尾契约（`test/unit/mock_connection.h`）

把契约写进 `pump_executor()` 的文档：**排空必须发生在最后一个 session 引用被
释放之前**，且必须是 `close()` + 排空成对（watchdog 的 async_wait 只有 close 能断）。

新增 `finish_session(session)` 收尾助手，一次做完三件事：close → 排空 →
**等引用计数收敛**。第三步是关键：`use_count()` 是"还有谁持有会话"的直接观测量
（队列 lambda / watchdog handler / worker 回调各持一份 self），计数收敛到 1 才说明
除调用方外再无持有者。这挡住了两类问题：worker 线程的迟到投递（#1 的 pop3 偶发），
以及"句柄复位了但回调还持着 session"（#2 的 UAF）。

调用点：pop3 / imaps / smtps 并发三个套件的 `~Handle()`（覆盖 33 + 46 + 9 个用例）、
framework 层 3 个测试的 close 块、imaps 里 2 个手工 session 用例。

### 3 + 4. 覆盖率链路（`CMakeLists.txt` + `test/scripts/coverage.sh`）

- `CMakeLists.txt` gcc 覆盖率分支加 `-fprofile-update=atomic`：计数器改成原子读改写。
- `coverage.sh` 的 `lcov --capture` 加 `--ignore-errors negative`：兜住头文件跨 TU
  合并那类伪影（也兜住 atomic 之外的残余成因）。
- `coverage.sh` 的 `lcov --remove` 加 `--ignore-errors unused`：模式是否命中随环境
  漂移，列表保留是为将来兜底，命中不到不应判失败。

## 验证（与 CI 同环境：`ubuntu:24.04` + gcc 13.3，非宿主 macOS）

| 检查 | 修前 | 修后 |
|---|---|---|
| `-DENABLE_ASAN=ON` + ctest | 18/23 | **23/23**（连跑 2 轮稳定） |
| 5 个泄漏用例单独跑 LSan | 全部报泄漏 | 全部干净 |
| pop3_fsm_test 反复 12 次 | 6 次里 2 次泄漏 | **12/12 干净** |
| 并发用例（coverage 构建）反复 10 次 | 必崩（SEGV） | **10/10 干净** |
| 并发用例（ASan+coverage）反复 10 次 | 6 次里 3 次非零退出 | **10/10 干净** |
| `test/scripts/coverage.sh --ci` | 死在 capture | **EXIT=0**：ctest 23/23 + capture/remove/summary/genhtml 全通，产出 HTML（行覆盖 48.1%） |
| 宿主 AppleClang ctest | — | **24/24** |

## 教训

1. **"测试挂红"必须当信号处理，不能当噪声**。红了 8 次 push 无人深究，代价是期间
   所有单测/消毒器信号全部失真——真回归混进来也看不出来。`unit-test / full-build /
   fuzz-smoke` 绿而 `asan-unit / coverage` 红，很容易被读成"只有消毒器和覆盖率有
   问题"，但这两者恰恰是抓内存问题的主力。
2. **报错信息会骗人，要顺着现象往上游走**。`Merge mismatch for function 7039` +
   `Error writing` 把我引向 libgcov；真相是 UAF 砸坏了 libgcov 的缓冲。转折点是
   "同一份代码无插桩时 5/5 干净"——**插桩只是改变了时序，不是故障来源**。
   组合消毒器（ASan + coverage）是拿到这类真相的最快手段。
3. **修完一个错误一定要把整条链路再跑一遍**。CI 的 coverage job 里还叠着两条被
   掩盖的故障（#2 的 SEGV、#4 的 lcov 报错），只修眼前那条，job 依然红。
4. **mock 的生命周期语义要显式成文**。这几条根因都在"mock 与生产行为不等价"的
   那部分（生产有常驻 IO 线程排空队列、有 server 兜底生命周期）。`get_executor()`
   那次（2026-09-04）也是同一类。凡是 mock 需要调用方额外做动作的地方，写成契约
   文档，别只靠局部注释。
5. **偶发泄漏要用观测量收敛，不要用"多等一会"**。`use_count() > 1` 是可直接观测的
   不变量，比 sleep 猜窗口可靠，也自带诊断价值（超时即说明还有持有者）。
6. **本地复现不了 ≠ 没问题，本地复现了 ≠ 根因在上面**。#3 本地复现不出（依赖负载
   时序）只能靠根因治理 + 兜底两道防线；#2 本地能复现但报错信息指向错误的方向。
   两条都说明：结论要落在证据上，而不是落在第一条能解释现象的假设上。
