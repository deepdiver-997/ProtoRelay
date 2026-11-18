# Local storage：共享记账与有界 commit 调度

## 范围与确认语义

本次实现只覆盖本地写入流的 `write_at/pwrite` 与 `commit/fsync`，不代表整台机器的磁盘负载，也不包含 sidecar、mmap、下载、其他进程或 distributed provider 的内部文件操作。

- 正文写入仍保持同步和单写者顺序，`pwrite` 成功不等于持久化成功。
- `commit_async` 可以投递到独立存储池；显式同步 `commit()` 仍在调用线程执行，使用同一准入与记账。
- 不取消真实 `fsync`，不提前返回 SMTP 最终 250；after_enqueue 也仍先完成正文 commit。
- 不含自动 inline 切换、自适应线程数、批量数据库事务、正文 group commit 或 native 连接分发修改。

## 共享边界

`pr::LocalStorageRuntime` 由 composition root 持有，经 `bootstrap::Options/Assembled.local_storage_runtime` 注入。主 local provider 和带 `storage_root` 的 local shards 共用同一 runtime；combined SMTP/IMAP 显式共享它。独立 assemble 默认彼此隔离，即使路径或 IO pool 一样，也不通过全局表猜测共享关系。

这不是全局 static 单例，更不是跨进程/全机器额度。需要多进程共同限速、自动按物理磁盘划分预算时，应另行设计协调机制。

不传 runtime 的直接构造路径保留原有内联行为，不会偷偷创建线程池。通用存储层不认识邮件对象 key/layout，应用层 local provider 继续转发原有 layout 与 metadata 能力。

## 配置

```json
{
  "storage": {
    "provider": "local",
    "local": {
      "mail_path": "./mail",
      "attachment_path": "./attachments",
      "commit_mode": "offload",
      "commit_threads": 2,
      "max_pending_commits": 64,
      "max_pending_commit_bytes": 67108864
    }
  }
}
```

- `commit_mode`：`offload`（应用装配默认）或显式 `inline`。
- `commit_threads`：offload 专用工作线程数，默认 2；inline 不创建专用工作线程，字段仍须为正。
- `max_pending_commits`：已接受但尚未完成交接的提交数上限，默认 64。
- `max_pending_commit_bytes`：上述提交对应的应用写入字节之和上限，默认 64 MiB；不是内存占用或设备脏页限制。
- 三个数值必须为正整数，不接受负数、0、浮点或类型/范围不合法的配置。
- 四个新参数为 startup-only。reload 改变它们会被拒绝并提示重启；注入共享 runtime 时，配置必须一致，不能由后装配的 server 静默修改共享池。

默认值是保守起点，不是经过硬件标定的最优参数。显式 inline 用于兼容、诊断和公平 A/B；队列满或关闭绝不隐式回退 inline fsync。

## 额度与状态

一个接受的提交经历：

```text
queued → running → completion_pending → 完成
```

- `outstanding = queued + running + completion_pending`，数量和字节都受同一次原子准入检查限制。
- fsync 实际结束后就释放 running 统计；完成 closure 尚未执行或用户 callback 尚未返回时，仍持有 outstanding 额度。
- 容量不足或关闭后返回 `IoError::retryable`，调用方可映射为 SMTP 451；不会执行被拒绝任务的 fsync，也不会在 I/O 线程等待额度。
- write 路径本轮只计量，不新增拒绝策略；write in-flight bytes 在本次逻辑操作返回时释放。
- `uncommitted_bytes` 累计实际写入后尚未确认 commit 的字节，在成功 fsync 或安全 abort 时移除。它只作观测，不作为可能让半封邮件停住的硬预算。

计数单位是逻辑 write/commit 操作，不是每次 EINTR/短写重试的 syscall 次数。写入失败时仍记录成功写出的部分字节。耗时使用 steady clock，分别记录 write 执行、commit 排队和 commit 执行的 count/total/max；这些是应用操作耗时，不是设备固件服务时间，也没有直接声称给出 p95/p99。

## 异步生命周期

写流使用独立 shared state；任务不捕获外层流裸指针。短锁只维护状态与账本，不包住 pwrite/fsync 或用户 callback。

- write/commit 同一流不并发发起；不合时宜的操作返回明确失败。
- 排队时 abort 可以关闭和删除尚未使用的文件；运行中的 pwrite/fsync 只设置 abort 请求，不通过跨线程 close 尝试取消 syscall。
- 成功 fsync 到结果发布之间有 `Persisted` 状态。此时 abort 仍可删除未发布对象；发布成功后才成为 `Committed`。
- 新增兼容的 `IWriteStream::commit_async_on(dispatcher, callback)`。旧后端无需覆写即可交接完成；本地 runtime 可跟踪至指定 executor 上的 callback 真正返回。
- dispatcher 必须最终执行/保留 closure 或抛出异常，不能静默丢弃。关闭 composition root 必须保持完成 executor 存活，否则 drain 无法替外部事件循环执行 closure。
- task、交接或 callback 异常均归还额度；重复或迟到 completion 不重复通知。异常交接只通知失败，不回退执行持久化。

SMTP commit pin 住本次 writer，先回连接 executor 检查连接、writer 身份和 mail ID，再发布结果和续跑 MIME。关闭或 reset 的旧任务先 abort 再完成交接，不会操作新邮件或留下未入队正文。无法投递到连接 executor 的异常路径不在存储 worker 上操作协议状态，也绝不继续返回 250。

## 关闭顺序

各入口使用同一个 `bootstrap::ShutdownScope`，由创建它的装配线程管理共享资源。不能在 storage task/callback 或 owner IO executor 上同步等待自身 drain。

1. request_stop，阻止新接入；同步停止 accept。
2. 通过弱会话 registry 在各 owner executor 上关闭已有会话并完成关闭屏障。
3. 关闭 local runtime 新准入，在 server、IO 和 worker 仍存活时 drain 已接受提交及完成交接，随后停专用存储池。
4. 停业务持久化/出站资源；排空 worker、IO，最后销毁 server。

shared runtime 只由共同装配根关闭，单个 provider/server 不擅自停止它。不使用固定 sleep 作为排空保证。运行中的系统调用不能硬取消，关闭会等待其返回。

## 观测

`MailServerBase::refresh_metrics()` 从 runtime snapshot 拉取 `protorelay_local_storage_*` 指标：

- write 操作数、成功/失败数、请求/写入字节、in-flight 操作和字节；
- uncommitted bytes；
- commit queued/running/completion_pending/outstanding 的操作数和字节；
- accepted/completed/failed/rejected、交接失败和 callback 异常；
- write、commit queue/service 的计时 count/纳秒 total/max，最老排队年龄；
- runtime 数量、准入状态与配置上限。

同一 runtime 跨 shard 去重；每个 metrics exporter 独立维护累计差量基线，重复 scrape 不重复计数，registry 更换时重建基线。commit completed 指存储任务及完成交接，不等同 SMTP 最终成功邮件数；业务接受数仍使用独立 SMTP 指标。

## 验证入口

- `local_storage_runtime_test`：原子容量、排队/执行/回调阶段、字节上限、重入、异常、关闭、实例隔离和最后 owner 析构。
- `local_file_write_runtime_test`：真实文件往返、运行中 abort 不关闭 fd、排队取消、发布前取消、拒绝后重试、短写/EINTR/部分写失败/fsync 失败、writer 保活。
- `local_storage_bootstrap_test`：配置、共享/隔离、reload、metrics 去重/重复 scrape、正常与异常关闭。
- `smtp_local_commit_test`：慢 fsync 期间 IO executor 可推进；提交前无 250；完成后 MIME 在 owner executor 运行；关闭/reset/失败不返回错误的成功确认。

使用实例级 `LocalFileWriteStream::Operations` 注入 syscall 闸门/错误来确定性验证交错；生产 provider 始终使用真实 pwrite/fsync，不用全局测试开关。

标准验证仍为 `./build.sh Release && bash test/run_all_tests.sh --no-build`。ASan/UBSan 使用独立 BUILD_DIR 和 `EXTRA_CMAKE_ARGS='-DENABLE_ASAN=ON'`，仍通过 build.sh 获取全局构建锁。性能对照必须在编译和消毒检查结束后串行运行，保持消息/确认语义相同并严格检查回复。
