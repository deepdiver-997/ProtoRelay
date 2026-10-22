#!/usr/bin/env python3
"""
IMAP IDLE 跨进程推送 E2E。

生产拓扑是 smtpsServer / imapsServer **两个独立进程**，所以"投递 → IDLE 收到
`* N EXISTS`"必须跨进程成立：IMAP 会话靠自己的轮询定时器查库发现新邮件，
进程内 IdlePushRegistry 只是同进程快路径。

本脚本用原始 socket 直连两个进程，验证：
  1. IMAP LOGIN → SELECT INBOX → IDLE，收到 "+ idling"
  2. 另一个进程（SMTP submission 口）AUTH 后投递一封本域邮件
  3. IDLE 期间（未发 DONE/NOOP）在超时内收到未打标签的 "* N EXISTS"
  4. DONE → tagged OK，轮询停止（之后不再有 EXISTS）

用法（在仓库根目录，配置已由 tools/agents/apply_agent_env.py 改成独占端口/库）：
  python3 test/e2e/test_idle_push.py
"""
import base64
import json
import os
import socket
import subprocess
import sys
import time

HOST = "127.0.0.1"
USER = "idle_push_e2e@scut.email"
PASSWORD = "idle_push_pass"
SUBJECT = "idle-push-e2e"


def _load_json(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def _find_port(cfg, kind, policy=None):
    for listener in cfg.get("listeners", []):
        if listener.get("type") != kind:
            continue
        if policy is not None and listener.get("auth_policy") != policy:
            continue
        return int(listener["port"])
    raise SystemExit(f"配置里找不到 {kind}/{policy} 监听器")


def _mysql(db_cfg, sql):
    cmd = ["mysql", "-h", str(db_cfg.get("host", "localhost")),
           "-u", str(db_cfg["user"]), str(db_cfg["database"]), "-e", sql]
    if db_cfg.get("password"):
        cmd[3:3] = [f"-p{db_cfg['password']}"]
    subprocess.run(cmd, check=True, capture_output=True, text=True)


def seed(db_cfg):
    """建用户 + INBOX（box_type=1：mailbox_repository 按 box_type 兜底解析 INBOX）。"""
    sql = (
        f"INSERT IGNORE INTO users (mail_address, password, name, status) "
        f"VALUES ('{USER}', '{PASSWORD}', 'idle push e2e', 1);"
        f"INSERT IGNORE INTO mailboxes (user_id, name, is_system, box_type) "
        f"SELECT id, 'INBOX', 1, 1 FROM users WHERE mail_address='{USER}';"
    )
    _mysql(db_cfg, sql)
    out = subprocess.run(
        ["mysql", "-h", str(db_cfg.get("host", "localhost")), "-u", str(db_cfg["user"]),
         "-N", "-B", str(db_cfg["database"]), "-e",
         f"SELECT id FROM users WHERE mail_address='{USER}'"],
        check=True, capture_output=True, text=True, env=_mysql_env(db_cfg))
    return int(out.stdout.strip())


def _mysql_env(db_cfg):
    env = dict(os.environ)
    if db_cfg.get("password"):
        env["MYSQL_PWD"] = str(db_cfg["password"])
    return env


def wait_port(port, timeout=15.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((HOST, port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.1)
    return False


class Imap:
    def __init__(self, port):
        self.sock = socket.create_connection((HOST, port), timeout=10)
        self.buf = b""
        self.greeting = self._until(b"\r\n")

    def _until(self, marker, timeout=10.0):
        deadline = time.time() + timeout
        while marker not in self.buf:
            remaining = deadline - time.time()
            if remaining <= 0:
                raise TimeoutError(f"等待 {marker!r} 超时，已收到: {self.buf[-200:]!r}")
            self.sock.settimeout(remaining)
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("IMAP 连接被关闭")
            self.buf += chunk
        out, self.buf = self.buf.split(marker, 1)
        return out + marker

    def cmd(self, tag, text):
        self.sock.sendall(f"{tag} {text}\r\n".encode())
        resp = b""
        while True:
            line = self._until(b"\r\n")
            resp += line
            upper = line.upper()
            if upper.startswith(b"*") or upper.startswith(b"+"):
                continue
            if upper.startswith(tag.encode().upper() + b" "):
                return resp.decode(errors="replace")

    def read_until(self, needle, timeout):
        return self._until(needle, timeout).decode(errors="replace")


def smtp_deliver(port, recipient):
    sock = socket.create_connection((HOST, port), timeout=10)
    sock.settimeout(10)
    # Message-ID 必须每次唯一：服务端按它去重，固定值会让第二封静默消失
    message_id = f"<idle-push-{time.time_ns()}@scut.email>"

    def talk(line, expect=None):
        if line is not None:
            sock.sendall((line + "\r\n").encode())
        resp = b""
        while b"\r\n" not in resp:
            resp += sock.recv(1024)
        text = resp.decode(errors="replace")
        if expect and not text.startswith(expect):
            raise RuntimeError(f"SMTP {line!r} → {text.strip()}")
        return text

    talk(None)                                   # banner
    talk("EHLO idle-push-e2e")
    token = base64.b64encode(("\0" + USER + "\0" + PASSWORD).encode()).decode()
    talk("AUTH PLAIN " + token, "235")
    talk(f"MAIL FROM:<{USER}>", "250")
    talk(f"RCPT TO:<{recipient}>", "250")
    talk("DATA", "354")
    body = (
        f"From: {USER}\r\n"
        f"To: {recipient}\r\n"
        f"Subject: {SUBJECT}\r\n"
        "Date: Thu, 22 Oct 2026 20:00:00 +0800\r\n"
        "Message-ID: " + message_id + "\r\n"
        "MIME-Version: 1.0\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "\r\n"
        "hello from the smtp process\r\n"
    )
    sock.sendall(body.encode() + b".\r\n")
    resp = b""
    while b"\r\n" not in resp:
        resp += sock.recv(1024)
    sock.sendall(b"QUIT\r\n")
    sock.close()
    return resp.decode(errors="replace").strip()


def _prepare_config(base_path, proj_root, port, auth_policy, mail_root):
    """从仓库配置派生临时配置：只留一个 TCP 监听、关掉 metrics。

    两个进程在同一台机器上跑，metrics 端口会撞；SSL 监听需要证书，
    e2e 只验 IDLE 推送，不需要它。
    """
    cfg = _load_json(base_path)
    cfg["listeners"] = [{"type": "tcp", "port": port, "auth_policy": auth_policy}]
    # 只要 certFile/keyFile 非空，transport 就会去初始化 TLS context（即使没有
    # SSL 监听）；本地 e2e 没有证书，清掉这两个键。
    cfg.pop("certFile", None)
    cfg.pop("keyFile", None)
    cfg["metrics_enabled"] = False
    cfg["log_to_console"] = False
    local = cfg.setdefault("storage", {}).setdefault("local", {})
    local["mail_path"] = os.path.join(mail_root, "mail")
    local["attachment_path"] = os.path.join(mail_root, "att")
    cfg["db_config_file"] = os.path.join(proj_root, "config", "db_config.json")
    path = os.path.join(mail_root, os.path.basename(base_path))
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(cfg, fh)
    return path


def main():
    proj_root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    os.chdir(proj_root)

    smtp_cfg = _load_json("config/smtpsConfig.json")
    imap_cfg = _load_json("config/imapsConfig.json")
    db_cfg = _load_json("config/db_config.json")

    smtp_port = _find_port(smtp_cfg, "tcp", "on")      # submission 口，允许 AUTH
    imap_port = _find_port(imap_cfg, "tcp")            # 143 analog

    run_root = os.path.join("/tmp", "protorelay_agent_1", "idle_push_e2e")
    os.makedirs(run_root, exist_ok=True)
    smtp_path = _prepare_config("config/smtpsConfig.json", proj_root, smtp_port, "on", run_root)
    imap_path = _prepare_config("config/imapsConfig.json", proj_root, imap_port, "off", run_root)

    print(f"== 播种用户 {USER} ==")
    user_id = seed(db_cfg)
    print(f"   user_id={user_id}")

    print("== 启动两个独立进程 ==")
    logs = {}
    procs = []
    for name, binary, cfg_path in (("smtp", "./build/smtpsServer", smtp_path),
                                   ("imap", "./build/imapsServer", imap_path)):
        log = open(f"/tmp/idle_push_{name}.log", "w")
        logs[name] = log
        procs.append(subprocess.Popen([binary, "-c", cfg_path], cwd=proj_root,
                                      stdout=log, stderr=subprocess.STDOUT,
                                      start_new_session=True))
    try:
        if not (wait_port(smtp_port) and wait_port(imap_port)):
            raise SystemExit("服务端口未就绪")
        print(f"   SMTP={smtp_port}  IMAP={imap_port}（两个独立进程）")

        imap = Imap(imap_port)
        print("== IMAP LOGIN / SELECT / IDLE ==")
        assert "OK" in imap.cmd("A1", f"LOGIN {USER} {PASSWORD}")
        select = imap.cmd("A2", "SELECT INBOX")
        before = 0
        for line in select.splitlines():
            if "EXISTS" in line:
                before = int(line.split()[1])
        print(f"   SELECT INBOX → {before} 封")

        imap.sock.sendall(b"A3 IDLE\r\n")
        idle_resp = imap.read_until(b"+ idling", 5)
        print(f"   {idle_resp.strip()!r}")

        print("== 另一个进程投递（SMTP） ==")
        started = time.time()
        delivery = smtp_deliver(smtp_port, USER)
        print(f"   {delivery}")

        print("== 等 IDLE 推送（未发 DONE/NOOP） ==")
        pushed = imap.read_until(b"EXISTS", 15)
        elapsed = time.time() - started
        print(f"   收到 {pushed.strip()!r}（投递后 {elapsed:.2f}s）")
        assert f"* {before + 1} EXISTS" in pushed, f"期望 * {before + 1} EXISTS，实际 {pushed!r}"

        print("== DONE / LOGOUT ==")
        imap.sock.sendall(b"DONE\r\n")
        done = imap.read_until(b"OK IDLE terminated", 5)
        print(f"   {done.strip()!r}")

        # DONE 后轮询必须停：再投一封，等 3 秒确认 socket 上没有任何未打标签
        # 响应（否则说明定时器没取消，会在非 IDLE 态乱推）。
        smtp_deliver(smtp_port, USER)
        imap.sock.settimeout(3.0)
        leaked = b""
        try:
            leaked = imap.sock.recv(4096)
        except socket.timeout:
            pass
        assert b"EXISTS" not in leaked, f"DONE 后仍在推送: {leaked!r}"
        print("   DONE 后 3s 无推送（轮询已停）")

        print(f"   {imap.cmd('A4', 'LOGOUT').strip()!r}")
        imap.sock.close()

        print("\nPASS: 跨进程 IDLE 推送生效")
        return 0
    finally:
        for proc in procs:
            proc.terminate()
        for proc in procs:
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        for log in logs.values():
            log.close()


if __name__ == "__main__":
    sys.exit(main())
