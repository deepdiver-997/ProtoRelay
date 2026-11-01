# OpenSSL 3.5 const getter：smtp_client_test 全新构建必挂

日期：2026-11-01　|　影响面：`test/unit/smtp_client_test.cpp`（仅测试，无生产代码）　|　状态：已修

## 症状

任何**全新构建目录**（新 worktree、CI 干净检出、TSan/覆盖度专用目录）配置后编译
`smtp_client_test` 直接失败：

```
X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, ...)
candidate function not viable: 1st argument ('const X509_NAME *') would lose const qualifier
```

为什么 main 上一直绿：hub 的 `build/` 里有旧缓存对象，该 TU 自 Homebrew OpenSSL
升级后从未被重编——断点只在"清空构建"的场景暴露。

## 根因

OpenSSL 3.5 起一批 getter 改为返回 const 指针（`X509_get_subject_name` 在列，
macOS Homebrew 头文件已带上）。`auto* name = X509_get_subject_name(cert)` 得到
`const X509_NAME*`，喂给需要 `X509_NAME*` 的 `X509_NAME_add_entry_by_txt` 丢
const。与 QUIC 模块已遇过的 `X509V3_EXT_conf_nid` 移除同族：**本机新 OpenSSL 头
文件 vs 旧调用姿势**。

## 修复

`test/unit/smtp_client_test.cpp` 的 `TlsFixture` 改为与版本无关的写法：
`X509_NAME_new()` 构造 → `add_entry_by_txt` → `X509_set_subject_name` /
`X509_set_issuer_name`（两者内部拷贝）→ 用完 `X509_NAME_free`。语义与原代码
等价（原来直接就地改 subject 再复制给 issuer），新旧 OpenSSL 头文件均编译通过。

## 教训

- "CI 绿"不等于"全新构建绿"：长寿命构建目录会掩蔽工具链升级带来的断点。
  今后新增依赖系统头文件行为的代码，值得在干净目录至少编一次
  （10-29 已有同类教训：GCC 容器构建才暴露的 FSM typename）。
- 修复姿势优先选 API 层面的跨版本等价写法，而不是 `const_cast`（后者只是把
  断点推迟到运行时 UB 边界）。
