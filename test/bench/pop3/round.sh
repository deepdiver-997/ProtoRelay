#!/bin/bash
# POP3 读路径基准 —— 脚本只管跑：端口/DB 全部读服务端配置。
#
# 用法:
#   USERS="test2@scut.email,t1@scut.email" test/bench/pop3/round.sh
#
# 可调（环境变量，均可缺省）:
#   CONFIG         服务端配置 JSON（端口取第一个 tcp listener）。多用户并发
#                  需要 DB-backed 配置（use_database=true），否则登录全失败
#   BUILD_DIR      二进制目录（默认 ./build）
#   USERS          逗号分隔用户列表；POP3 单会话锁：并发连接数必须 ≤ 用户数
#   PASS           所有用户共用密码（默认 test123）
#   MATRIX         ramp 的 "线程 并发" 对（默认 "1 1 4 1 5 1"）
#   ROUNDS         每连接轮数（默认 2000）
#   MAILS          每轮 RETR 1:N（默认 5）
#   LOCK_CLEANUP   轮间清 pop3_session_lock（默认 1）。压测客户端跑完不发 QUIT
#                  直接关 socket，锁要等心跳清扫（~5min）；不清则后续轮全 IN-USE。
#                  DB 连接参数读 CONFIG 的 db_config_file，密码经 MYSQL_PWD 环境传
#   CLEAR_LOCKS_NOW=1  只清锁退出（不做基准）
#
# 注意: 请用独立测试库；登录属 bcrypt 校验，首登 rehash 已在 warmup 覆盖。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$ROOT"

CONFIG="${CONFIG:-config/pop3Config.json}"
BUILD_DIR="${BUILD_DIR:-./build}"
SERVER="$BUILD_DIR/pop3Server"
CLIENT="$BUILD_DIR/pop3_client"
USERS="${USERS:-test2@scut.email}"
PASS="${PASS:-test123}"
MATRIX="${MATRIX:-1 1 4 1 5 1}"
ROUNDS="${ROUNDS:-2000}"
MAILS="${MAILS:-5}"
LOCK_CLEANUP="${LOCK_CLEANUP:-1}"

[[ -x "$SERVER" && -x "$CLIENT" ]] || { echo "缺二进制: 先 ./build.sh Release" >&2; exit 1; }

eval "$(python3 - "$CONFIG" <<'PY'
import json, os, shlex, sys
c = json.load(open(os.path.abspath(sys.argv[1])))
port = next(l["port"] for l in c["listeners"] if l.get("type") == "tcp")
dbf = c.get("db_config_file") or ""
if dbf and not os.path.isabs(dbf):
    dbf = os.path.join(os.path.dirname(os.path.abspath(sys.argv[1])), dbf)
print("PORT=" + shlex.quote(str(port)))
print("DB_CONFIG=" + shlex.quote(dbf))
print("USE_DB=" + shlex.quote("1" if c.get("use_database") else "0"))
PY
)"
echo "POP3 round: config=$CONFIG port=$PORT users=[$USERS] db=$DB_CONFIG"
[[ "$USE_DB" == 1 || -z "$DB_CONFIG" ]] || true
if [[ "$USE_DB" != 1 ]]; then
    echo "⚠ 配置未开 use_database：登录将全部失败，多用户基准需要 DB-backed 配置" >&2
fi

# ── 锁清理：读 db_config_file 拿连接参数，密码走 MYSQL_PWD 环境 ──────────
clear_locks() {
    [[ "$LOCK_CLEANUP" == 1 && -n "$DB_CONFIG" ]] || return 0
    DB_CONFIG="$DB_CONFIG" DB_PASS="${DB_PASS:-}" python3 - <<'PY'
import json, os, subprocess, sys
c = json.load(open(os.environ["DB_CONFIG"]))
env = dict(os.environ)
if c.get("password"): env["MYSQL_PWD"] = c["password"]
cmd = ["mysql", "-h", str(c.get("host", "localhost")), "-P", str(c.get("port", 3306)),
       "-u", c["user"], c["database"], "-e", "DELETE FROM pop3_session_lock;"]
r = subprocess.run(cmd, env=env, capture_output=True, text=True)
if r.returncode != 0:
    print("锁清理失败（继续，可能 IN-USE）: " + r.stderr.strip()[:120], file=sys.stderr)
PY
}

[[ "${CLEAR_LOCKS_NOW:-0}" == 1 ]] && { clear_locks; echo "locks cleared"; exit 0; }

SRV_LOG="${TMPDIR:-/tmp}/pop3_round_server.log"
stop_srv() { pkill -f "$SERVER" 2>/dev/null || true; sleep 1; }
start_srv() {
    ( "$SERVER" -c "$CONFIG" >> "$SRV_LOG" 2>&1 & )
    for _ in $(seq 1 20); do
        sleep 0.5
        lsof -nP -iTCP:"$PORT" -sTCP:LISTEN >/dev/null 2>&1 && return 0
    done
    echo "服务端 10s 内未监听 $PORT，查看 $SRV_LOG" >&2; exit 1
}
trap 'stop_srv' EXIT

warmup() {
    USERS="$USERS" PASS="$PASS" PORT="$PORT" python3 - <<'PY'
import os, socket
port = int(os.environ["PORT"])
for u in filter(None, os.environ["USERS"].split(",")):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.recv(200)
    s.sendall(f"USER {u}\r\n".encode()); s.recv(200)
    s.sendall(f"PASS {os.environ['PASS']}\r\n".encode()); r = s.recv(200).decode(errors="replace")
    s.sendall(b"QUIT\r\n"); s.close()
    print(f"warmup {u}: {'OK' if r.startswith('+OK') else r.strip()[:50]}")
PY
}

start_srv
warmup
set -- $MATRIX
while [[ $# -ge 2 ]]; do
    "$CLIENT" --port "$PORT" --users "$USERS" --t "$1" --conns "$2" --rounds "$ROUNDS" --mails "$MAILS" || true
    clear_locks
    sleep 1
    shift 2
done

stop_srv
echo ""
echo "POP3 round complete."
