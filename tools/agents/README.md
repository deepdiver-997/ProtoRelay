# 多 agent 并行工作区（tools/agents/）

每个 agent 一个 `git worktree`，各有自己的构建目录、配置副本、非标端口段、`/tmp` 根目录和 MySQL 库；
`build.sh` 的全局锁保证整台机器同一时刻只编译一个。人只在 hub 里讨论方向、合并、收尾。
agent 会话要遵守的规则在仓库根的 [`AGENTS.md`](../../AGENTS.md)。

本方案参考 `sbs-local` 的夜跑模式，只移植了 worktree 管理 + 构建/资源隔离，**没有**任务看板、任务领取与自动立项——
本项目的任务大多需要讨论才能定方向。

## 布局

```
mail-system/
├── v8/                               ← hub：人的集成检出（本仓库）
│   ├── build.sh                      内置全局构建锁、ccache、worktree 生产拦截
│   ├── AGENTS.md                     agent 会话规则
│   ├── tools/agents/
│   │   ├── new-worktree.sh           开工作区（worktree + 播种配置 + 分配资源 + 改写配置 + 建库）
│   │   ├── remove-worktree.sh        收工作区（带未提交/未合并保护）
│   │   ├── status.sh                 状态面板
│   │   ├── db_init.sh                为 agent 建/删独立库
│   │   ├── apply_agent_env.py        把 .agent-env 写进 worktree 自己的配置副本
│   │   ├── lib.sh                    公共库
│   │   ├── agents.conf.example       本机参数模板（agents.conf 不入库）
│   │   └── README.md                 本文
│   └── .git/agents/                  ← 跨 worktree 共享的运行态（永不入库、不会被 build.sh clean 清掉）
│       ├── build.lock/               全局构建锁（pid / worktree / cmd / since）
│       ├── build-history.tsv         每次构建一行
│       ├── alloc/<name>.env          资源分配记录
│       └── STOP                      存在时 worktree 内新构建一律拒绝（紧急刹车）
└── agents/                           ← AGENTS_DIR（默认 hub 父目录下的 agents/）
    └── <name>/                       worktree，分支 feat/<name>
        ├── build/                    自己的构建目录
        ├── config/                   从 hub 播种的 gitignored 配置（已改成独占端口/库名）
        ├── test/config/              用独占端口/目录生成的测试配置
        └── .agent-env                身份 + 资源（gitignored）
```

## 生命周期

### 1. 开（人在 hub）

```bash
tools/agents/new-worktree.sh smtp-dedup                 # 基于 main
tools/agents/new-worktree.sh k2 --from feature/x --build Release
```

做的事：`git worktree add ../agents/<name> -b feat/<name> main` → 拷 hub 里 gitignored 的 `config/`
文件（`db_config.json`、`smtpsConfig.json`、证书、DKIM…；缺证书则自签）→ 分配 `AGENT_INDEX`/端口段/tmp/库名 →
`apply_agent_env.py` 改写 worktree 自己的 `config/*.json`、`test/config/*_test.json`（被跟踪的几份标
`skip-worktree`，端口改动不会出现在 `git status`）→ 用独占端口重生成 `test/config` → `db_init.sh` 建
`mail_agent_N` 并建表（失败只警告）。

然后把 worktree 路径交给 agent 会话，让它读 `AGENTS.md` 开工。

### 2. 用（agent 在 worktree）

```bash
cd ../agents/smtp-dedup
./build.sh Release                       # 排队全局锁
bash test/run_all_tests.sh --no-build    # 日志在 build/test-logs/
git commit ...                           # 只提交到 feat/smtp-dedup
```

### 3. 并（人在 hub）

```bash
tools/agents/status.sh                   # 看谁脏、谁领先、谁拿着锁
git merge --no-ff feat/smtp-dedup
./build.sh Release && bash test/run_all_tests.sh --no-build
```

### 4. 收（人在 hub）

```bash
tools/agents/remove-worktree.sh smtp-dedup                      # 未提交/未合并会被拒
tools/agents/remove-worktree.sh smtp-dedup --drop-db --yes      # 连独立库一起删
tools/agents/remove-worktree.sh smtp-dedup --force --delete-branch
```

## 资源分配（.agent-env）

| 字段 | 含义 | 默认规则 |
|---|---|---|
| `AGENT_INDEX` | 1..99 最小空闲 | 释放后可复用 |
| `AGENT_PORT_BASE` | 独占端口段起点 | `20000 + INDEX*100`；段内任一端口被占（别的项目/别的 agent）则整段后移 |
| `AGENT_PORT_SMTP_TCP/SSL/SUBMISSION` | 替代测试端口 2525/8465/8587 | base+0/1/2 |
| `AGENT_PORT_IMAP_TCP/SSL` | 替代 1414/8993 | base+3/4 |
| `AGENT_PORT_METRICS/WEBMAIL/E2E_IMAP` | 替代 9090/8081/10143 | base+5/6/7 |
| `AGENT_PORT_SMTP/SMTPS/SUBMISSION/IMAP/IMAPS/POP3/POP3S/REGISTER` | 替代生产风格 25/465/587/143/993/110/995/8080 | base+10..17 |
| `AGENT_TMP_ROOT` | `/tmp` 下独占根目录 | `/tmp/protorelay_agent_<INDEX>` |
| `AGENT_DB` | 独立 MySQL 库 | `mail_agent_<INDEX>`（账号沿用共享测试账号） |
| `AGENT_TABLE_PREFIX` | 表前缀，备用 | `a<INDEX>_`（目前各库独立，表名不需要前缀） |
| `PROTORELAY_TEST_TMP_ROOT` / `PROTORELAY_TEST_PORT_BASE` | 喂给 `setup_test_env.py` | 由上面推导 |

`apply_agent_env.py` 的改写规则：`listeners[].port`、`metrics_port`、`metrics.port` 按上表映射；
`log_file`、`storage.local.mail_path/attachment_path` 中以 `/tmp/` 开头的改到 `AGENT_TMP_ROOT` 下；
`address` 0.0.0.0 → 127.0.0.1；`db_config.json.database` → `AGENT_DB`。**`outbound.*` 是外发目标端口，不改。**

## 什么能并行、什么不能

| 动作 | 跨 worktree 并行 | 说明 |
|---|---|---|
| 编辑代码 / git 操作 | ✅ | 各自 worktree，互不可见 |
| `./build.sh` | ✅（自动排队） | 全局锁串行化编译；默认 jobs 按内存/CPU 自动算 |
| 单测 `test/run_all_tests.sh` | ⚠️ | 日志已隔离到 `build/test-logs/`，但 `test/unit/*` 内约 25 处硬编码 `/tmp/xxx`，**同一个单测二进制不要同时在两个 worktree 跑** |
| 起服务 / 集成测试 / e2e | ⚠️ | 端口、tmp、库已隔离；但 `integration_test.sh` 等脚本仍有约定，**同一时刻一个 worktree** |
| MySQL | ✅ | 各用 `mail_agent_N`；共享 `mail` 库只读 |
| `cross-x64` / `object-only` / `sync-sysroot` / docker / deploy | ❌ | worktree 内被 `build.sh` 拒绝（退出码 77）；属于生产部署链 |

## 全局构建锁（build.sh 内置）

- `mkdir .git/agents/build.lock` 原子抢锁，锁内记 `pid` `worktree` `cmd` `since`。
- 被占时每 30s 打印一次持有者；`BUILD_LOCK_WAIT`（默认 3600s）超时退出 75。
- 持有者进程已退出且锁龄 >60s 自动清扫；Ctrl-C / 失败都会放锁（统一 EXIT trap）。
- 无人争用时立即获取、退出即释放——单人使用与以前完全一致。`BUILD_LOCK_DISABLE=1` 可跳过（仅调试）。
- `touch .git/agents/STOP` 让所有 worktree 的新构建立刻被拒（hub 不受影响），`rm` 解除。

## ccache

装了 `ccache` 时，`build.sh` 对**新建的**构建目录（每个新 worktree 都是）自动加 `*_COMPILER_LAUNCHER=ccache`，hub 与所有
worktree 共享用户级缓存，新 worktree 的冷构建大部分命中。hub 里已有的老构建目录默认不碰（启用会触发一次全量重编），
想让它们也用：`USE_CCACHE=1 ./build.sh Release`（一次性）；`USE_CCACHE=0` 关闭并撤掉 launcher。
默认缓存上限是 5 GiB，多个 worktree 并用建议放大：`ccache -M 20G`。

## 本机参数

`cp tools/agents/agents.conf.example tools/agents/agents.conf` 后按需改：`AGENTS_DIR`、`BASE_BRANCH`、
`BUILD_LOCK_WAIT`、`USE_CCACHE`、`PORT_BLOCK_START/SIZE`、`TMP_ROOT_PREFIX`、`DB_NAME_PREFIX`。

## 排错

| 现象 | 处理 |
|---|---|
| `Waiting for the global build lock` 很久 | `tools/agents/status.sh` 看持有者；进程确实没了就等 60s 自动清扫，或 `rm -rf .git/agents/build.lock` |
| `Refusing ... from an agent worktree`（退出 77） | 你在 worktree 里跑了 cross-x64/object-only/sync-sysroot，去 hub 跑 |
| 退出 76 | `.git/agents/STOP` 存在，操作者按了刹车 |
| worktree 里改了 `config/pop3Config.json` 却 `git status` 看不到 | 它被标了 skip-worktree；`git update-index --no-skip-worktree <file>` 后再提交 |
| 新 worktree 没有 `tools/agents/`、`AGENTS.md` | 这些文件还没合进基准分支；脚本一律用 hub 的副本操作 worktree，不依赖 worktree 里有它们 |
| `db_init.sh` 建库失败 | 用户没有 CREATE 权限：由管理员 `CREATE DATABASE mail_agent_N; GRANT ALL ON mail_agent_N.* TO 'mail_test'@'%';` 后重跑 |
| 端口段分配跳过了默认段 | 段内有端口被别的进程占着（探测用 127.0.0.1 bind），属正常后移 |

## 与 sbs-local 夜跑的差异

- 没有看板 / `taskctl` / `claim.sh` / `author.sh`：任务由人与 agent 讨论后直接下达。
- 构建锁放在 `build.sh` 本体而非外层 wrapper：`test/run_all_tests.sh`、`deploy.sh` 等一切入口都被覆盖；
  并发槽固定为 1（本项目编译重、机器还跑别的项目，串行几乎不亏）。
- 共享运行态放 `.git/agents/` 而非 `build/night/`：`build.sh clean` 不会把锁状态一起删掉。
- 多了资源申请器（端口段 / tmp / 库名）与配置改写：本项目的集成测试要起真实服务，sbs-local 不需要。
- 多了 worktree 内的生产拦截：本项目有真实部署链，sbs-local 没有。
