# AGENTS.md — 本仓库 AI 编码会话须知

> 适用于在本仓库任何检出里工作的 agent 会话：hub 主检出（`v8/`）或 `../agents/<项目名>/<name>` 下的 worktree（带项目命名空间）。
> 多 agent 并行方案的设计、流程与排错见 [`tools/agents/README.md`](tools/agents/README.md)。

## 1. 先确认你在哪

- `cat .agent-env` 存在 → 你在一个 **agent worktree** 里。文件里是你的名字、分支、独占端口段、tmp 目录、库名。- 不存在 → 你在 **hub**（人的集成检出）。hub 里不要起服务、不要开长任务；需要并行工作时请人用
  `tools/agents/new-worktree.sh <name>` 开一个 worktree 再做。

## 2. 边界

- 只改自己 worktree 里的文件；不 `cd` 到 hub 或别的 worktree 去改、去跑。
- 提交到自己的 `feat/<name>` 分支。**不合并 main、不 push、不加远端。** 合并由人在 hub 里 `git merge --no-ff feat/<name>` 完成。
- 新文件记得 `git add`。根 `.gitignore` 有 `*.sh` 全局忽略：新 shell 脚本要么加 `!` 例外，要么 `git add -f`，并在汇报里说明。
- `config/*.json`、`test/config/*_test.json` 里被标了 **skip-worktree** 的几份是你的本地端口/库名改写，`git status` 看不到、也提交不了；
  真要改这些文件的内容再提交，先 `git update-index --no-skip-worktree <file>`。

## 3. 构建与测试

- 构建一律 `./build.sh <Debug|Release>`；不要手敲 `cmake --build` 另起并行编译。
  `build.sh` 内置**全局构建锁**：hub + 所有 worktree 同一时刻只跑一个编译。看到
  `Waiting for the global build lock ...` 是正常排队，不是故障。
- 完工前必须绿：`./build.sh Release && bash test/run_all_tests.sh --no-build`。单测日志在 `build/test-logs/`。
- 单测会写固定的 `/tmp/xxx` 路径：**不要**与别的 worktree 同时跑同一个单测二进制。

## 4. 运行服务 / 集成测试 / 数据库

- 用 `.agent-env` 分配给你的端口（全部非标，20000+）、`AGENT_TMP_ROOT`、`AGENT_DB`。
  `new-worktree.sh` 已把它们写进你 worktree 的 `config/*.json` 和 `test/config/*.json`；
  e2e 脚本（`test/e2e/*.py`）的 `--port` / `--server` 参数需要你显式传。
- 跑 `setup_test_env.py` / `cleanup_test_env.sh` 前先 `set -a; . .agent-env; set +a`（它们读 `PROTORELAY_TEST_*`）。
- 集成测试（`integration_test.sh`、e2e）同一时刻只允许一个 worktree 在跑；要跑先在对话里说。
- 独立库 `mail_agent_N` 随便造数据；**绝不**向共享的 `mail` 库写入。

## 5. 生产红线（硬规则）

- 不跑 `cross-x64` / `object-only` / `sync-sysroot`（`build.sh` 在 worktree 内会直接拒绝，退出码 77）。
- 不碰 `deploy.sh`、`docker/`、`Dockerfile`、`deploy/`、任何 `/opt/*` 路径。
- 不连任何非 `127.0.0.1` / `localhost` 的地址——包括 `test/scripts/test_ports.sh`、`test_auth.sh` 这类对真实端口探测的脚本。
- 不改 `config/*.example.json` 里生产域名、密钥路径的语义。

## 6. 资源上限（无人监管会话必读）

- 本项目存在**并行模式**（`tools/agents/`）。有人在旁边监管的普通任务可以直接在 hub 做；
  **无人监管的任务必须走并行模式**（`tools/agents/new-worktree.sh` 开工作区），不许直接占着 hub 跑长任务。
- 开工作区前必须有**设备级磁盘预算**（全机所有项目共享一份，`tools/agents/budget.sh show` 可查）。
  未设置时 `new-worktree.sh` 会直接拒绝；预算由人显式 `budget.sh set <GB>` 设定且经过 df 实测校验，
  **不是每个项目各领一份**——账本在 `~/.agent-workspaces/`，跨项目合计。
- 编译并行度用默认值（`build.sh` 按内存推导，per-target 默认 `-j4`），无头会话不要手动调高
  （数字参数 / `SERIAL_JOBS`）；全局构建锁保证 hub + 所有 worktree 同时只有一个编译。
- 不无限制消耗资源：不开 docker、不并行跑多个集成测试、不把测试数据灌进共享 `mail` 库、
  worktree 收尾（合并或废弃）后立刻 `remove-worktree.sh` 释放，不留构建副本。

## 7. 状态落盘与汇报

- 上下文可能被压缩：进度写进 commit / 分支，不靠记忆。一个任务对应一个（或少数几个）语义清晰的 commit。
- 完工在对话里汇报：分支名、`git log --oneline main..HEAD`、`test/run_all_tests.sh` 结果、遗留问题。
  本项目没有任务看板：方向靠与人讨论得出，不自行立项。
