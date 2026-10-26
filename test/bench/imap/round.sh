#!/bin/bash
# IMAP 读路径基准 —— 脚本只管跑：端口/DB/存储全部读服务端配置。
#
# 用法:
#   USER=test2@scut.email PASS=test123 test/bench/imap/round.sh
#
# 可调（环境变量，均可缺省）:
#   CONFIG            服务端配置 JSON（端口取第一个 tcp listener）
#   BUILD_DIR         二进制目录（默认 ./build）
#   USER / PASS       压测账号（默认 test2@scut.email / test123）
#   WARMUP_USERS      预热登录列表，逗号分隔（默认 $USER）。账号密码为明文存储时
#                     首登会触发 rehash-to-bcrypt，必须预热后再测
#   MATRIX            FETCH ramp 的 "线程 并发" 对列表（默认 "1 1 4 1 16 1 16 4"）
#   ROUNDS            每连接轮数（默认 2000）
#   MAILS             FETCH 1:N（默认 200，须与邮箱实际规模一致，否则按有的算）
#   SELECT_ONLY       跑不跑 SELECT-only 对照（默认 1）
#   SELECT_MATRIX     SELECT-only 的 "线程 并发" 对（默认 "4 1 16 1"）
#   SELECT_ROUNDS     SELECT-only 每连接轮数（默认 50000，它快得多）
#
# 注意: 读路径不产生数据，可直接重复跑；DB 请指向独立测试库。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$ROOT"

CONFIG="${CONFIG:-test/config/imaps_real.json}"
BUILD_DIR="${BUILD_DIR:-./build}"
SERVER="$BUILD_DIR/imapsServer"
CLIENT="$BUILD_DIR/imap_client"
USER="${USER:-test2@scut.email}"
PASS="${PASS:-test123}"
WARMUP_USERS="${WARMUP_USERS:-$USER}"
MATRIX="${MATRIX:-1 1 4 1 16 1 16 4}"
ROUNDS="${ROUNDS:-2000}"
MAILS="${MAILS:-200}"
SELECT_ONLY="${SELECT_ONLY:-1}"
SELECT_MATRIX="${SELECT_MATRIX:-4 1 16 1}"
SELECT_ROUNDS="${SELECT_ROUNDS:-50000}"

[[ -x "$SERVER" && -x "$CLIENT" ]] || { echo "缺二进制: 先 ./build.sh Release" >&2; exit 1; }

eval "$(python3 - "$CONFIG" <<'PY'
import json, os, shlex, sys
c = json.load(open(os.path.abspath(sys.argv[1])))
port = next(l["port"] for l in c["listeners"] if l.get("type") == "tcp")
print("PORT=" + shlex.quote(str(port)))
PY
)"

SRV_LOG="${TMPDIR:-/tmp}/imap_round_server.log"
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
    WARMUP_USERS="$WARMUP_USERS" PORT="$PORT" PASS="$PASS" python3 - <<'PY'
import os, socket, sys
port = int(os.environ["PORT"])
for u in filter(None, os.environ["WARMUP_USERS"].split(",")):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.recv(200)
    s.sendall(f"w1 LOGIN {u} {os.environ.get('PASS','')}\r\n".encode())
    r = s.recv(500).decode(errors="replace")
    s.sendall(b"w2 LOGOUT\r\n"); s.close()
    print(f"warmup {u}: {'OK' if ' OK ' in r else r.strip()[:60]}")
    if " OK " not in r: sys.exit(1)
PY
}

start_srv
warmup
A=(--port "$PORT" --user "$USER" --pass "$PASS" --mails "$MAILS")
echo ""
echo "=== FETCH 1:$MAILS ramp (rounds=$ROUNDS) ==="
set -- $MATRIX
while [[ $# -ge 2 ]]; do
    "$CLIENT" "${A[@]}" --t "$1" --conns "$2" --rounds "$ROUNDS"
    shift 2
done

if [[ "$SELECT_ONLY" == 1 ]]; then
    echo ""
    echo "=== SELECT-only 对照 (rounds=$SELECT_ROUNDS) ==="
    set -- $SELECT_MATRIX
    while [[ $# -ge 2 ]]; do
        "$CLIENT" "${A[@]}" --t "$1" --conns "$2" --rounds "$SELECT_ROUNDS" --select-only
        shift 2
    done
fi

stop_srv
echo ""
echo "IMAP round complete."
