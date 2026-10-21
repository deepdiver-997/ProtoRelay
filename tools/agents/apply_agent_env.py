#!/usr/bin/env python3
"""apply_agent_env.py — 把 .agent-env 里分配到的独占资源写进 worktree 自己的配置副本。

只改 worktree 内的文件，绝不碰 hub：
  config/db_config.json                    database → AGENT_DB
  config/smtpsConfig.json / imapsConfig.json / pop3Config.json / webmailConfig.json
  test/config/smtps_test.json / imaps_test.json
      listeners[].port、metrics_port、metrics.port   按下面的端口映射表平移到独占端口段
      log_file、storage.local.mail_path/attachment_path  以 /tmp/ 开头的改到 AGENT_TMP_ROOT 下
      address 0.0.0.0 → 127.0.0.1（agent 的服务只在本机回环可见）
  outbound.* 是外发目标端口，绝不改。

映射表（标准/测试端口 → .agent-env 字段）：
  2525→SMTP_TCP  8465→SMTP_SSL  8587→SMTP_SUBMISSION  1414→IMAP_TCP  8993→IMAP_SSL
  9090→METRICS   8081→WEBMAIL   10143→E2E_IMAP
  25→SMTP  465→SMTPS  587→SUBMISSION  143→IMAP  993→IMAPS  110→POP3  995→POP3S  8080→REGISTER

用法：apply_agent_env.py <worktree_root> [--env <path>] [--dry-run]
退出码：0 成功；1 参数/文件错误。被改动的文件会逐个打印。
"""
import argparse
import json
import os
import sys

PORT_MAP = {
    2525: "AGENT_PORT_SMTP_TCP",
    8465: "AGENT_PORT_SMTP_SSL",
    8587: "AGENT_PORT_SMTP_SUBMISSION",
    1414: "AGENT_PORT_IMAP_TCP",
    8993: "AGENT_PORT_IMAP_SSL",
    9090: "AGENT_PORT_METRICS",
    8081: "AGENT_PORT_WEBMAIL",
    10143: "AGENT_PORT_E2E_IMAP",
    25: "AGENT_PORT_SMTP",
    465: "AGENT_PORT_SMTPS",
    587: "AGENT_PORT_SUBMISSION",
    143: "AGENT_PORT_IMAP",
    993: "AGENT_PORT_IMAPS",
    110: "AGENT_PORT_POP3",
    995: "AGENT_PORT_POP3S",
    8080: "AGENT_PORT_REGISTER",
}

SERVER_CONFIGS = [
    "config/smtpsConfig.json",
    "config/imapsConfig.json",
    "config/pop3Config.json",
    "config/webmailConfig.json",
    "test/config/smtps_test.json",
    "test/config/imaps_test.json",
]
DB_CONFIGS = ["config/db_config.json"]


def load_env(path):
    env = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            v = v.strip()
            if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
                v = v[1:-1]
            env[k.strip()] = v
    return env


def remap_port(port, env, changes, where):
    key = PORT_MAP.get(port)
    if key and key in env:
        new = int(env[key])
        if new != port:
            changes.append(f"{where}: {port} → {new}")
        return new
    return port


def remap_tmp_path(value, env, changes, where):
    root = env.get("AGENT_TMP_ROOT")
    if not root or not isinstance(value, str) or not value.startswith("/tmp/"):
        return value
    if value == root or value.startswith(root.rstrip("/") + "/"):
        return value  # 已经在独占根目录下，保持幂等
    new = os.path.join(root, value[len("/tmp/"):])
    changes.append(f"{where}: {value} → {new}")
    return new


def patch_server_config(cfg, env, changes):
    if cfg.get("address") == "0.0.0.0":
        cfg["address"] = "127.0.0.1"
        changes.append("address: 0.0.0.0 → 127.0.0.1")
    for i, lst in enumerate(cfg.get("listeners") or []):
        if isinstance(lst, dict) and isinstance(lst.get("port"), int):
            lst["port"] = remap_port(lst["port"], env, changes, f"listeners[{i}].port")
    if isinstance(cfg.get("metrics_port"), int):
        cfg["metrics_port"] = remap_port(cfg["metrics_port"], env, changes, "metrics_port")
    metrics = cfg.get("metrics")
    if isinstance(metrics, dict) and isinstance(metrics.get("port"), int) and metrics["port"] != 0:
        metrics["port"] = remap_port(metrics["port"], env, changes, "metrics.port")
    if "log_file" in cfg:
        cfg["log_file"] = remap_tmp_path(cfg["log_file"], env, changes, "log_file")
    local = (cfg.get("storage") or {}).get("local")
    if isinstance(local, dict):
        for k in ("mail_path", "attachment_path"):
            if k in local:
                local[k] = remap_tmp_path(local[k], env, changes, f"storage.local.{k}")


def patch_db_config(cfg, env, changes):
    db = env.get("AGENT_DB")
    if db and cfg.get("database") != db:
        changes.append(f"database: {cfg.get('database')} → {db}")
        cfg["database"] = db
    # initialize_script 保持原样：MySQLPool::execute_sql_script 会按 database 字段自行
    # CREATE DATABASE IF NOT EXISTS + USE，并忽略脚本里残留的选库语句，服务启动即在 AGENT_DB 内建表。


def process(root, rel, env, patch, dry_run):
    path = os.path.join(root, rel)
    if not os.path.exists(path):
        return None
    with open(path) as f:
        cfg = json.load(f)
    changes = []
    patch(cfg, env, changes)
    if not changes:
        print(f"  [=] {rel} (无需改动)")
        return False
    print(f"  [~] {rel}")
    for c in changes:
        print(f"        {c}")
    if not dry_run:
        with open(path, "w") as f:
            json.dump(cfg, f, indent=2, ensure_ascii=False)
            f.write("\n")
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("worktree", help="worktree 根目录")
    ap.add_argument("--env", default=None, help=".agent-env 路径（默认 <worktree>/.agent-env）")
    ap.add_argument("--dry-run", action="store_true", help="只打印不写")
    a = ap.parse_args()

    root = os.path.abspath(a.worktree)
    env_path = a.env or os.path.join(root, ".agent-env")
    if not os.path.isdir(root):
        print(f"worktree 不存在: {root}", file=sys.stderr)
        return 1
    if not os.path.exists(env_path):
        print(f".agent-env 不存在: {env_path}", file=sys.stderr)
        return 1
    env = load_env(env_path)
    for must in ("AGENT_PORT_BASE", "AGENT_TMP_ROOT", "AGENT_DB"):
        if must not in env:
            print(f".agent-env 缺少 {must}", file=sys.stderr)
            return 1

    print(f"[apply_agent_env] worktree={root}  port_base={env['AGENT_PORT_BASE']}  "
          f"tmp={env['AGENT_TMP_ROOT']}  db={env['AGENT_DB']}{'  (dry-run)' if a.dry_run else ''}")
    touched = []
    for rel in DB_CONFIGS:
        if process(root, rel, env, patch_db_config, a.dry_run):
            touched.append(rel)
    for rel in SERVER_CONFIGS:
        if process(root, rel, env, patch_server_config, a.dry_run):
            touched.append(rel)
    # 供调用方（new-worktree.sh）决定哪些被跟踪文件要标 skip-worktree
    print("TOUCHED=" + " ".join(touched))
    return 0


if __name__ == "__main__":
    sys.exit(main())
