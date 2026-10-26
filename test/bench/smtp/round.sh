#!/bin/bash
# SMTP 投递路径四象限基准 —— 脚本只管跑：端口/存储路径/性能开关全部读服务端配置。
#
# 用法:
#   CONFIG=test/config/smtps_mock.json test/bench/smtp/round.sh
#
# 可调（环境变量，均可缺省）:
#   CONFIG          服务端配置 JSON；端口取第一个 tcp listener，存储取
#                   storage.local.mail_path（象限间会清空该目录）
#   ACK_MODE        覆盖 inbound_ack_mode（after_enqueue|after_persist）
#   INFLIGHT_CAP    覆盖 persist_max_inflight_mails（perf_mode 默认会抬到 1M，
#                   要测背压必须显式给一个小值，如 8）
#   BUILD_DIR       二进制目录（默认 ./build）
#   MSGS_PERCONN    per-conn 象限每线程消息数（默认 5000，受 ephemeral 池约束）
#   MSGS_REUSE      reuse 象限每线程消息数（默认 50000）
#   T_PERCONN       per-conn 线程 ramp（默认 "1 2 4 8 16"）
#   T_REUSE         reuse 线程 ramp（默认 "1 2 4 8 16 32 64"）
#   LOCAL_IPS       逗号分隔回环地址，透传 --local-ips（每 worker 绑一个 IP 扩
#                   四元组空间，每 IP 独立 16K 端口池）。地址需先挂 lo0 别名
#                   （一次性，重启失效）；预检发现缺别名会退出并打印补挂命令
#   QUADRANTS       跑哪些象限（默认 "1 2 3 4"；只关心满吞吐给 "4"）
#   SKIP_DRAIN=1    跳过 per-conn 象限后的 TIME_WAIT 排空（不推荐）
#
# 输出: 各组合一行 ok/fail/elapsed/rate，人肉誊进 REPORT.md（本脚本不改报告）。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$ROOT"

CONFIG="${CONFIG:-test/config/smtps_mock.json}"
CONFIG="$(python3 -c "import os,sys;print(os.path.abspath(sys.argv[1]))" "$CONFIG")"
BUILD_DIR="${BUILD_DIR:-./build}"
SERVER="$BUILD_DIR/smtpsServer"
CLIENT="$BUILD_DIR/smtp_client"
MSGS_PERCONN="${MSGS_PERCONN:-5000}"
MSGS_REUSE="${MSGS_REUSE:-50000}"
T_PERCONN="${T_PERCONN:-1 2 4 8 16}"
T_REUSE="${T_REUSE:-1 2 4 8 16 32 64}"
SKIP_DRAIN="${SKIP_DRAIN:-0}"
QUADRANTS="${QUADRANTS:-1 2 3 4}"

[[ -x "$SERVER" && -x "$CLIENT" ]] || { echo "缺二进制: 先 ./build.sh Release" >&2; exit 1; }
[[ -f "$CONFIG" ]] || { echo "配置不存在: $CONFIG" >&2; exit 1; }

# ── 从配置读资源占用（端口 / 存储路径），按需生成覆盖配置 ────────────────
eval "$(CONFIG_PATH="$CONFIG" ACK_MODE="${ACK_MODE:-}" INFLIGHT_CAP="${INFLIGHT_CAP:-}" python3 - <<'PY'
import json, os, shlex, tempfile
cfg_path = os.environ["CONFIG_PATH"]
ack, cap = os.environ.get("ACK_MODE", ""), os.environ.get("INFLIGHT_CAP", "")
c = json.load(open(cfg_path))
port = next(l["port"] for l in c["listeners"] if l.get("type") == "tcp")
mail_dir = c["storage"]["local"]["mail_path"]
overrides = {}
if ack:
    c["inbound_ack_mode"] = ack
    overrides["inbound_ack_mode"] = ack
if cap:
    c["persist_max_inflight_mails"] = int(cap)
    overrides["persist_max_inflight_mails"] = int(cap)
if overrides:
    cfg_path = os.path.join(tempfile.gettempdir(), "smtp_round_config.json")
    json.dump(c, open(cfg_path, "w"), indent=2)
for k, v in (("PORT", port), ("MAIL_DIR", mail_dir), ("EFF_CONFIG", cfg_path)):
    print(f"{k}={shlex.quote(str(v))}")
print("EFFECTIVE=" + shlex.quote(json.dumps(overrides, ensure_ascii=False)))
PY
)"
echo "SMTP round: config=$EFF_CONFIG port=$PORT storage=$MAIL_DIR overrides=$EFFECTIVE"
case "$MAIL_DIR" in ""|"/"|"$HOME") echo "拒绝清空可疑存储目录: $MAIL_DIR" >&2; exit 1;; esac

# ── LOCAL_IPS 预检：缺别名直接退出并给出补挂命令（不做开机持久化） ────────
LOCAL_IPS="${LOCAL_IPS:-}"
IPS_ARGS=()
if [[ -n "$LOCAL_IPS" ]]; then
    ok_ips=(); missing=()
    IFS=',' read -ra cand <<< "$LOCAL_IPS"
    for ip in "${cand[@]}"; do
        if python3 - "$ip" <<'PY'
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
try: s.bind((sys.argv[1], 0))
except OSError: sys.exit(1)
s.close()
PY
        then ok_ips+=("$ip"); else missing+=("$ip"); fi
    done
    if [[ ${#missing[@]} -gt 0 ]]; then
        echo "以下回环地址缺别名（${#missing[@]} 个），无法扩四元组空间，退出：" >&2
        echo "  ${missing[*]}" >&2
        echo "补挂（一次性，重启失效；每 IP 独立扩 16K 端口池）：" >&2
        for ip in "${missing[@]}"; do
            echo "  sudo ifconfig lo0 alias $ip 255.255.255.255 up" >&2
        done
        echo "或批量补挂 2..17:" >&2
        echo '  for i in $(seq 2 17); do sudo ifconfig lo0 alias 127.0.0.$i 255.255.255.255 up; done' >&2
        echo "不挂别名也可去掉 LOCAL_IPS 用 127.0.0.1 继续（per-conn t≥8 会受端口池限制）。" >&2
        exit 1
    fi
    IPS_ARGS=(--local-ips "$(IFS=,; echo "${ok_ips[*]}")")
elif [[ " $QUADRANTS " == *" 1 "* || " $QUADRANTS " == *" 3 "* ]]; then
    echo "提示: per-conn 象限受单 IP 16K 端口池限制（≈546 连接/秒/IP，t≥8 会塌）。"
    echo "      挂别名后可用 LOCAL_IPS=127.0.0.2,127.0.0.3,... 破除，详见 smtp/REPORT.md。"
fi

# ── 服务端生命周期 ──────────────────────────────────────────────────────
SRV_LOG="${TMPDIR:-/tmp}/smtp_round_server.log"
stop_srv() { pkill -f "$SERVER" 2>/dev/null || true; sleep 1; }
start_srv() {
    ( "$SERVER" -c "$EFF_CONFIG" >> "$SRV_LOG" 2>&1 & )
    for _ in $(seq 1 20); do
        sleep 0.5
        if lsof -nP -iTCP:"$PORT" -sTCP:LISTEN >/dev/null 2>&1; then return 0; fi
    done
    echo "服务端 5s 内未监听 $PORT，查看 $SRV_LOG" >&2; exit 1
}
wipe_storage() { rm -rf "$MAIL_DIR"; mkdir -p "$MAIL_DIR"; }
tw() { netstat -an | grep -c TIME_WAIT 2>/dev/null || echo 0; }
drain() {
    (( SKIP_DRAIN == 1 )) && return 0
    echo "  -- 排空 TIME_WAIT（<50 后再静置 35s）..."
    while [ "$(tw)" -gt 50 ]; do sleep 3; done
    sleep 35
}
trap 'stop_srv' EXIT

ramp() {
    local label="$1" msgs="$2" threads="$3"; shift 3
    echo ""
    echo "============================================================"
    echo "  $label  (msgs=$msgs, port=$PORT)"
    echo "============================================================"
    for t in $threads; do
        "$CLIENT" "$@" --t "$t" --msgs "$msgs" --port "$PORT" "${IPS_ARGS[@]}" || true
    done
}

stop_srv; wipe_storage; start_srv
has_q() { [[ " $QUADRANTS " == *" $1 "* ]]; }
has_q 1 && ramp "1. sequential + per-conn" "$MSGS_PERCONN" "$T_PERCONN" --seq
if has_q 2; then stop_srv; wipe_storage; drain; start_srv
    ramp "2. sequential + reuse" "$MSGS_PERCONN" "$T_REUSE" --seq --reuse; fi
if has_q 3; then stop_srv; wipe_storage; start_srv
    ramp "3. pipeline + per-conn" "$MSGS_PERCONN" "$T_PERCONN" --pipe; fi
if has_q 4; then stop_srv; wipe_storage; drain; start_srv
    ramp "4. pipeline + reuse" "$MSGS_REUSE" "$T_REUSE" --pipe --reuse; fi
stop_srv; wipe_storage
echo ""
echo "SMTP round complete."
