# 2026-10-22 生产启动 SEGV：孤儿 .o 的 ODR 符号劫持

## 现象

62 个重构提交（af5e40e..6ed83a9，IMAP 缺陷修复 + persistence/storage 边界重构）在
macOS 全量 62/62 ctest 通过后，按 deploy 流程交叉编译、服务器链接出的二进制
**smtpsServer 与 imapsServer 双双启动即 SEGV**（systemd 重启循环）。

崩溃前日志（两个服务完全一致的签名）：

```
test/server/smtps_test.cpp:136      Loaded config file: /opt/smtpServer/config/smtpsConfig.json
src/framework/thread_pool/io_thread_pool.cpp:47  Starting IOThreadPool...
src/mail_system/back/bootstrap.cpp:60  Thread pools ready (io=4 worker=4)
src/mail_system/back/bootstrap.cpp:69  Database pool initialized successfully
（此后无任何日志 → signal=SEGV）
```

即崩溃点夹逼在 bootstrap.cpp 第 69 行之后、第 150 行（"Shard router initialized"）
之前：`build_main_storage → ensure_ready → HashShardRouter` 一小段。生产已回滚到
bak-20261022-084719，现役服务恢复健康。

## 取证（服务器重放，全部可复现）

按 deploy 第 4 步同款配方在服务器重放链接（新文件，不碰现役）：

1. **gdb 栈顶**：主线程 SEGV 于
   `pr::storage::LocalFileStorageProvider::ensure_ready(pr::IoError&)+61`，
   故障指令 `mov 0x8(%rbx),%rbp`，`rbx=0x15`（把小整数当指针解引用）。
   反汇编确认 `rbx` 就是 `this->directories_.begin()`——**向量头本身是垃圾**，
   对象从未被正确构造。附带证据：把生产 config 复制改端口后连 `db_config.json`
   都找不到（走 NullDBPool）也照样崩——**与数据库无关**。
2. **差分链接**：同一批对象，仅剔除 3 个孤儿 .o（见下）后重链，二进制干净启动
   越过崩溃点并稳定运行；仅剔除 2 个 storage 孤儿亦然（session 孤儿无害）。
3. **符号解析证据**：崩溃二进制中
   `mail_system::storage::LocalFileStorageProvider::LocalFileStorageProvider(string,string)`
   解析到 **size 0x2b9 的强符号（T）**——正是孤儿对象里的旧版构造函数；
   现行 bootstrap.cpp.o 里同名符号是 size 0x6c5 的弱符号（新包装类 inline 构造，
   带 `_ZThn8_`/`_ZThn40_` 双继承 thunk）。ELF 规则**强符号覆盖弱符号，与顺序无关**。

## 根因链

1. **孤儿累积**：2026-08 的重构（984842a）删除/搬移了
   `src/mail_system/back/storage/{local,distributed}_file_storage_provider.cpp`。
   CMake 增量构建**不会清理**已删源文件的 .o，它们留在 `build/cross-x64/CMakeFiles/`
   里，并被 build.sh 的镜像导出原样带进 `artifacts/.../obj/`（Release/Asan/SafeRelease
   四棵树全部中招）；rsync `--link-dest` 又把孤儿持续同步到服务器 obj 树。
2. **同名异体**：2026-10-22 的 56b5856 把应用层包装类
   `mail_system::storage::LocalFileStorageProvider` 改成了**双继承新布局**
   （`pr::storage::LocalFileStorageProvider` + `IMailObjectKeyLayout`，两个 vptr、
   新成员 `layout_`、thunk 偏移 8/40），mangled 名与两个月前的旧类完全相同——
   孤儿里的旧定义从"冗余但兼容"变成"同名异体"，ODR 地雷被引爆。
3. **劫持生效**：link.sh 按 `--obj-root` 全量收集 .o，孤儿混入 smtps/imaps 两个
   生产链接。运行时 `make_shared<mail_system::storage::LocalFileStorageProvider>`
   按**新 sizeof** 分配内存，却执行**旧构造函数**（强胜弱）：旧构造函数按旧布局
   写自己的两个成员、完全不构造 `pr::storage::` 基子对象——`directories_` 向量
   从未初始化。随后 `main_storage->ensure_ready()` 经 vtable（该符号只有现行代码
   定义，未被污染）正常派发进现行 `pr::storage::LFSP::ensure_ready`，第一次解引用
   垃圾 begin 指针 → SEGV。日志因此停在 :69 与 :150 之间。
4. **为什么 macOS/容器单测不炸**：ctest 与本地可执行链接走 CMake target，只链接
   **当前源码清单**里的对象，孤儿根本不进链接；只有 deploy 的 link.sh 按 obj 根
   全量收集。同理，重构前的生产二进制也不炸——孤儿内容与当时现行代码一致，
   只是冗余。`imapsServer` 同链同崩（journal 同款签名）也印证了链路级根因。

## 修复（双保险 + 回归测试）

1. `build.sh`：object-only 导出后剪枝——导出树里每个 `.o` 必须能在当前检出找到
   对应源文件，否则删除（根因侧：孤儿不再可能进产物）。
2. `link.sh` 新增 `--collect-dir <dir>`（可重复）：收集范围收窄到显式列出的对象树，
   并强制其在 `--obj-root` 之下。`deploy.sh` 链接改为
   `--collect-dir …/smtpsServer_obj.dir`（smtps）与
   `imapsServer_obj.dir + smtpsServer_obj.dir`（imaps，仍需 `--exclude smtps_test`），
   不再依赖 `--exclude` 子串去挡 webServer/combined 树（结构性防线）。
3. 回归测试 `object_tree_hygiene`（`test/backend_contract/check_object_tree_hygiene.cmake`，
   注册进两条 ctest 分支）：扫描 `artifacts/*/*/obj/CMakeFiles` 下所有 .o，
   源文件不存在的孤儿一票否决。无产物树时平凡通过。

## 验证

- 服务器重放：剪枝树 + `--collect-dir` 链出的 smtpsServer.fixed / imapsServer.fixed
  均打印 `bootstrap.cpp:150 Shard router initialized`（崩溃点之后的日志）并稳定运行
  至超时；含孤儿的原链接 100% 复现 SEGV。
- 本地：`cmake -P check_object_tree_hygiene.cmake` 对未剪枝产物树 FAIL（列出全部
  孤儿，含 Asan/SafeRelease 树），剪枝后 PASS。
- macOS Debug 全量构建 + 全量 ctest、Ubuntu 22.04 容器全量构建 + ctest：绿。

## 教训

- 部署链接按目录**全量收集**是对 ODR 不设防的：任何"同 mangled 名、不同定义"的
  历史对象都可能被链入并以强符号劫持现行弱符号，且只炸生产不炸单测。
- 重构删除/搬移源文件后，构建目录里的旧 .o 是隐形炸弹；对象树卫生必须成为
  构建与 CI 的显式不变量。
