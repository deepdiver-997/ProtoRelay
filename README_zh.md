# ProtoRelay

ProtoRelay 是一个基于 C++20 的邮件中继核心，当前聚焦在 SMTP 协议执行与投递链路基础能力。

## 当前已实现范围

目前项目有意保持边界清晰，重点完成 SMTP 核心三件事：

- SMTP 状态机（会话生命周期与命令流转）
- SMTP 解析（命令、信封、正文处理）
- 投递链路（队列与外发 relay 主路径）

这是一种分阶段策略：先把 relay 核心打牢，再逐步扩展更多协议与能力。

## 可扩展性设计

ProtoRelay 按模块抽象构建，而不是把逻辑耦合在单体里：

- 数据库连接池抽象（`mysql`、`mysql_distributed`）
- 存储适配器抽象（`local`、`distributed`、`hdfs_web`）
- 出站投递与 DNS 路由模块
- 配置驱动的启动装配

因此新增 provider/策略时，通常不需要改 SMTP FSM 核心路径。

## 命令行风格（向大项目靠拢）

当前 CLI 约定：

- `--help` / `-h`：统一帮助输出
- `--version` / `-V`：输出构建时注入的版本信息
- `--config` / `-c <path>`：显式指定配置文件
- 保留兼容：单个位置参数 `config_path`

示例：

```bash
./test/smtpsServer --help
./test/smtpsServer --version
./test/smtpsServer --config config/smtpsConfig.json
```

## 构建时版本注入

版本信息在 CMake 配置阶段自动生成并注入二进制，包含：

- 语义化版本号
- Git 短提交号
- UTC 构建时间
- 构建目标（OS-ARCH）
- 编译器信息
- 关键特性开关

## 编译

```bash
./build.sh Debug
./build.sh Release
```

脚本会自动创建 `build/`，并尽量避免在源码根目录产生构建垃圾文件。

## 环境依赖（摘要）

- Linux/macOS
- CMake 3.10+
- GCC 9+ / Clang 10+
- Boost、OpenSSL、MySQL client、spdlog、c-ares
- 如启用 `hdfs_web` 存储：额外需要 `libcurl`

## 项目规范文档

见：

- `docs/PROJECT_STYLE.md`

## 许可证

MIT；Boost 许可证见 `COPYING_BOOST.txt`。
