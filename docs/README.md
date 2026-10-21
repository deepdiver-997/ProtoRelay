# ProtoRelay 文档

## 目录结构

```
docs/
├── README.md                          # 本文件（主索引）
├── mail-system/                       # ★ 邮件系统业务（协议/架构/bug）
│   ├── README.md                      # 业务索引 + 阅读顺序
│   ├── protocol-flow.md               # 协议流程入门（新人先读这篇）
│   ├── architecture/                  # 邮件业务架构
│   │   ├── pop3-server-design.md      # POP3 服务器设计
│   │   ├── imap-server-design.md      # IMAP 服务器设计
│   │   ├── imap-protocol-flow.md      # IMAP 协议流程
│   │   ├── smtp-outbound-client-design.md # SMTP 发件引擎设计
│   │   ├── inbound-verification-flow.md   # 入站校验流程
│   │   ├── mailbox-concurrency.md     # 多客户端并发对照（SMTP/POP3/IMAP）
│   │   ├── webmail-api.md             # HTTP API、SPA、附件和标记
│   │   ├── webmail-sessions-and-submission.md # 单会话与直接提交
│   │   ├── smtp-client-core.md         # 共享 SMTP 内核及边界
│   │   └── vs-postfix.md              # 与 Postfix 的对比
│   └── bugfixes/                      # 邮件业务 bug 复盘（YYYY-MM-DD-描述.md）
│       ├── 2026-08-27-counter-triangle-bug.md
│       ├── 2026-08-21-mail-body-rotation-fix.md
│       └── ...
├── architecture/                      # 整体架构 + 框架/基础设施
│   ├── ARCHITECTURE.md                # 整体架构
│   ├── architecture-evolution.md      # 架构演进历史
│   ├── framework-refactor.md          # 框架重构记录
│   ├── backend-decoupling.md          # 日志/Worker/网络后端边界与构建验证
│   ├── sharding-refactor.md           # 分片重构
│   └── storage-abstraction.md         # 存储抽象
├── build-deploy/                      # 构建、部署、运维
│   ├── cross-compile-guide.md         # 交叉编译指南
│   ├── docker-hdfs-web-guide.md       # Docker + HDFS Web 指南
│   ├── log-transform-toolchain.md     # 日志变换工具链
│   ├── logging-guide.md               # 日志指南
│   ├── operations.md                  # 运维文档
│   └── quick-log-config.md            # 快速日志配置
├── bugfixes/                          # 框架级 bug（编译器 / DB 连接池）
│   ├── arm-apple-clang-make-shared-sigbus.md
│   ├── prepared-statement-connection-pool-issue.md
│   ├── 2026-09-04-mock-empty-executor-fsm-tests.md
│   ├── 2026-09-05-stmt-fetch-data-truncated.md
│   └── 2026-09-16-ci-asan-leak-and-coverage-negative-count.md
├── reports/                           # 测试覆盖率报告（量化成果）
│   └── coverage-YYYY-MM-DD.md         # 由 test/scripts/coverage.sh 生成
└── style/                             # 规范与总结
    ├── PROJECT_STYLE.md               # 项目编码规范
    └── dev-summary.md                 # 开发总结
```

## 当前实现入口（2026-10-20）

SMTP/IMAP/POP3、共享 SMTP 内核、进程内提交与 React Webmail 均已落地。
API/会话以邮件业务架构文档为准；前端运行见 [webmail/README.md](../webmail/README.md)，
生产部署见 [operations.md](build-deploy/operations.md)，实际收发验证和已知衔接缺口见
[2026-10-18 发布验证](reports/release-2026-10-18.md)。通用 DB/存储/cache/HTTP 位于
`framework/`，应用旧路径保留兼容 shim。带日期的报告、复盘及阶段记录反映当时状态，
历史测试数、旧类名和故障示例不代表当前能力。

本次框架重构的提交、接口、线程拓扑和依赖边界见
[日志、Worker 与网络后端解耦](architecture/backend-decoupling.md)。现有 Asio 后端继续可用，
日志与 Worker 可选择标准库实现；独立契约构建不代表完整邮件服务已经可以去掉 Asio。

## 阅读指引

- **不熟悉邮件协议**：先读 `mail-system/protocol-flow.md`，再按
  `mail-system/README.md` 的顺序深入。
- **找某协议设计**：进 `mail-system/architecture/`。
- **找某次事故复盘**：`mail-system/bugfixes/`（业务）或 `bugfixes/`（框架）。
- **找覆盖率数据**：`reports/`。
- **多个 AI agent 并行开发**：根目录 [`AGENTS.md`](../AGENTS.md)（agent 会话规则）与
  [`tools/agents/README.md`](../tools/agents/README.md)（worktree / 构建锁 / 端口与库隔离的设计和日常流程）。

新 bugfix 文档按 `YYYY-MM-DD-简短描述.md` 格式命名；邮件业务相关的放
`mail-system/bugfixes/`，框架级的放 `bugfixes/`。

## 测试覆盖率报告

`test/scripts/coverage.sh` 一键测量单测的源码行覆盖率，生成 `docs/reports/coverage-<日期>.md`
（模块汇总 + framework 层逐文件 + 最低覆盖 Top15）与 `build-cov/html/`（HTML 详情）。
CI 的 coverage job 也会产出并上传 HTML artifact。

## 一次性任务文档

**任务/TODO 类文档不要放进 `docs/`**（尤其 `architecture/`，那里只放长期有效的设计）。
放到仓库根目录 `tasks/`（已被 `.gitignore` 排除，不纳入版本控制），任务做完即删。
经验总结如需保留，可在 `tasks/` 里写笔记，或提炼到 `docs/` 的长期文档中。
