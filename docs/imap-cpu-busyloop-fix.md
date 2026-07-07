# IMAP CPU 100% 空转 Bug 分析报告

## 现象

IMAP 服务进程（imapsServer）的两个 IO 线程各占用约 100% CPU，累计 CPU 时间远超正常水平（运行 5 小时累积 3 天 CPU 时间）。

## 根因 1：IDLE 模式流水线循环 O(n²) 空转

### 位置

`include/mail_system/back/mailServer/session/imaps_session.tpp` — `process_read()` IDLE 分支

### 触发条件

客户端发送 `IDLE` 命令进入 IDLE 状态后，如果缓冲区中有多个非 `DONE` 行（如管道化的后续命令或垃圾数据），会触发死循环。

### 调用链

```
async_read 回调
  └→ 数据追加到 command_read_buffer_
  └→ while (has_buffered_input())              ← 流水线消费循环
       ├→ handle_read(take_buffered_input())    ← 全量取出再全量放回（拷贝整个缓冲区）
       └→ process_read()
            └→ IDLE 分支：处理 1 行
                 ├→ 非 DONE → do_async_read()
                 │            └→ has_buffered_input() → true → 立即返回
                 └→ return
       └→ has_buffered_input() → true → 继续循环  ← 又回到 process_read
```

### 原因

旧代码在 IDLE 模式下每次只处理**一行**。处理完后调用 `do_async_read()` 期望等待新数据，但 `do_async_read()` 发现缓冲区还有 `\n` 就立即返回。流水线外层循环再次调用 `handle_read(take_buffered_input())` 完整拷贝剩余缓冲，然后再次进入 `process_read`。每轮迭代拷贝整个剩余缓冲区，时间复杂度 O(n²)。

### 修复

将 IDLE 模式下的逐行处理改为内部 `while(true)` 循环，一次性消费缓冲区中所有行：

```cpp
// 修复前：每次只处理 1 行
if (session->context_.idle_mode) {
    size_t line_end = buf.find("\r\n");
    if (line_end != std::string::npos) {
        // 处理 1 行
        if (upper == "DONE") { ... return; }
    }
    self->do_async_read();  // ← 回到流水线循环
    return;
}

// 修复后：批量消费所有行
if (session->context_.idle_mode) {
    while (true) {
        size_t line_end = buf.find("\r\n");
        if (line_end == std::string::npos) {
            self->do_async_read();  // 缓冲区无完整行，真正等待新数据
            return;
        }
        // 提取行、删除
        if (upper == "DONE") { ... return; }
        // 非 DONE → 丢弃，继续循环
    }
}
```

---

## 根因 2：`has_buffered_input()` 与 `process_read()` 行结束符不一致

### 位置

`include/mail_system/back/mailServer/session/session_base.h` — `has_buffered_input()`

### 触发条件

客户端发送的数据包含 `\n` 但不含 `\r\n`（如 Unix 风格换行、垃圾数据）。

### 调用链

```
async_read 回调
  └→ has_buffered_input() → true   ← buffer 中有 \n
  └→ handle_read(...)
  └→ process_read()
       └→ buf.find("\r\n") → npos   ← 但找不到 \r\n！
       └→ do_async_read()
            └→ has_buffered_input() → true  ← 又回到原点
            └→ return
       └→ return
  └→ has_buffered_input() → true   ← 死循环
```

### 原因

`has_buffered_input()` 只检查 `\n`，而 `process_read()` 需要 `\r\n` 才能形成完整命令行。当缓冲区中有 `\n` 但无 `\r\n` 时，两者判断不一致形成死循环：
- `has_buffered_input()` 认为"有数据可处理" → 触发流水线循环
- `process_read()` 认为"没有完整行" → 调用 `do_async_read()` 等待
- `do_async_read()` 又检查 `has_buffered_input()` → true → 立即返回

### 修复

将 `has_buffered_input()` 的检查从 `\n` 改为 `\r\n`，与 `process_read()` 对齐：

```cpp
// 修复前
virtual bool has_buffered_input() const {
    return command_read_buffer_.find('\n') != std::string::npos;
}

// 修复后
virtual bool has_buffered_input() const {
    return command_read_buffer_.find("\r\n") != std::string::npos;
}
```

SMTP 和 IMAP 协议规范（RFC 5321、RFC 3501）均要求 CRLF 行结束符，此改动不会影响正常客户端。

---

## 影响范围

- **根因 1**：仅影响 `ImapsSession::process_read()` 的 IDLE 分支
- **根因 2**：影响所有使用 `SessionBase` 的会话（SMTP + IMAP），但只在缓冲区有异常数据时触发

## 验证

修复后在服务器上执行完整 IMAP 会话（CAPABILITY → LOGIN → SELECT → IDLE/DONE → FETCH → LOGOUT），所有线程 CPU 保持 0%。
