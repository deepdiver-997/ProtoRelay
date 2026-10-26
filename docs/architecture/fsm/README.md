# 协议 FSM 拓扑图

由 `include/framework/fsm/fsm_export.h` 从各协议的 desc 纯数据生成
（fsm-extract 拓扑导出任务，Phase 1 静态导出）。图 = 表里**声明**的全部守卫行，
同键多出口（如 SMTP AUTH 的成功/失败分叉）如实并排画出，不推测运行时走向。

| 文件 | 说明 |
|---|---|
| `pop3.dot` / `pop3.mmd` | POP3（RFC 1939） |
| `smtps.dot` / `smtps.mmd` | SMTP TCP 变体（allow_starttls=true；SSL 连接无 STARTTLS 行） |
| `imap.dot` / `imap.mmd` | IMAP（RFC 3501，双终态 LOGOUT/CLOSED） |

`.mmd` 是 Mermaid `stateDiagram-v2`，GitHub/文档工具直接渲染；`.dot` 用
graphviz 渲染：`dot -Tpng pop3.dot -o pop3.png`。

## 再生成

协议表改动后重跑（测试内置开关，指向本目录即覆盖）：

```sh
FSM_EXPORT_OUT=docs/architecture/fsm ./build/fsm_export_test
```
