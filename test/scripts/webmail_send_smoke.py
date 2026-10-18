#!/usr/bin/env python3
"""独立 Python email 消费者验证 MIME；可选本地 DB + 双节点会话/提交/读信闭环。

python3 test/scripts/webmail_send_smoke.py --build-dir build-release
.venv/bin/python test/scripts/webmail_send_smoke.py --db-config config/db_config.json
后者需要 mysql-connector-python/bcrypt；使用临时高端口，仅创建/清理随机测试账号与邮件。
"""
import argparse
import http.cookiejar
import hashlib
import json
from pathlib import Path
import secrets
import socket
import smtplib
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from email import policy
from email.parser import BytesParser


def check_mime(build_dir, temp):
    path = temp / "sample.eml"
    subprocess.run([str(build_dir / "smtp_submitter_test"), "--emit-mime", str(path)], check=True)
    raw = path.read_bytes()
    message = BytesParser(policy=policy.default).parsebytes(raw)
    assert not message.defects
    assert str(message["Subject"]) == "你好，Webmail！" * 20
    assert message["From"].addresses[0].addr_spec == "sender@test.local"
    assert message["Cc"].addresses[0].addr_spec == "cc@test.local"
    assert message["Bcc"] is None
    assert message.get_content_type() == "multipart/alternative"
    parts = list(message.iter_parts())
    assert len(parts) == 2
    assert parts[0].get_content() == "你好\r\n.\r\nlast"
    assert parts[1].get_content() == "<p>你好 &amp; hello</p>"
    assert all(len(line) <= 998 for line in raw.split(b"\r\n"))
    assert b"\n" not in raw.replace(b"\r\n", b"")
    print("PASS: independent Python email parser: UTF-8 subject, folding, MIME alternatives, body roundtrip")


def check_local_e2e(build_dir, db_config_path, temp):
    import bcrypt
    import mysql.connector

    db_config = json.loads(db_config_path.read_text())
    if db_config.get("host") not in ("localhost", "127.0.0.1", "::1"):
        raise ValueError("This smoke test only supports a local database")
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        smtp_port = probe.getsockname()[1]
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        second_port = probe.getsockname()[1]
    db = mysql.connector.connect(
        **{key: db_config[key] for key in ("host", "port", "user", "password", "database") if key in db_config},
        connection_timeout=3, autocommit=True,
    )
    suffix = secrets.token_hex(8)
    addresses = [f"wm-{suffix}-{role}@webmail.test" for role in ("sender", "to", "cc")]
    alias = f"wm-{suffix}-group@webmail.test"
    password = secrets.token_urlsafe(24)
    hashed = bcrypt.hashpw(password.encode(), bcrypt.gensalt(rounds=4)).decode()
    processes, logs = [], []
    def new_client():
        jar = http.cookiejar.CookieJar()
        return urllib.request.build_opener(urllib.request.ProxyHandler({}),
            urllib.request.HTTPCookieProcessor(jar)), jar

    opener, cookie_jar = new_client()
    base = f"http://127.0.0.1:{port}"

    def api(path, body=None, client=None, content_type="application/json", node=None):
        request = urllib.request.Request((f"http://127.0.0.1:{node}" if node else base) + path,
            data=None if body is None else json.dumps(body).encode(),
            headers={} if body is None else {"Content-Type": content_type})
        try:
            response = (client or opener).open(request, timeout=35)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            raw = response.read()
            try:
                return response.code, json.loads(raw)
            except json.JSONDecodeError as error:
                raise RuntimeError(f"Unexpected HTTP response: {path}, status={response.code}, body={raw[:200]!r}") from error

    def wait_ready(probe):
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if any(p.poll() is not None for p in processes):
                raise RuntimeError("smoke server exited; inspect its temporary log")
            try:
                if probe():
                    return
            except (OSError, urllib.error.URLError):
                pass
            time.sleep(0.05)
        raise TimeoutError("smoke server did not become ready")

    def smtp_ready():
        with socket.create_connection(("127.0.0.1", smtp_port), timeout=0.5):
            return True

    try:
        with db.cursor() as cursor:
            for address in addresses:
                cursor.execute("INSERT INTO users (mail_address,password,name,status) VALUES (%s,%s,%s,1)",
                               (address, hashed, "webmail smoke"))
            for address in addresses[1:]:
                cursor.execute("INSERT INTO mail_aliases (alias_address,target_address,is_active) VALUES (%s,%s,1)", (alias,address))
        common = {
            "address": "127.0.0.1", "system_domain": "webmail.test", "use_database": True,
            "db_config_file": str(db_config_path), "io_thread_count": 2, "worker_thread_count": 2,
            "perf_mode": True, "metrics_enabled": False, "log_level": "warn",
            "log_to_console": True, "log_to_file": False, "intrusion_detection_enabled": False,
            "storage": {"provider": "local", "local": {
                "mail_path": str(temp / "mail"), "attachment_path": str(temp / "attachments")}},
        }
        smtp_config = {**common, "listeners": [{"type": "tcp", "port": smtp_port, "auth_policy": "on", "advertise_starttls": False}],
                       "smtp_daily_send_limit": 1, "external_delivery_enabled": False,
                       "persist_min_available_memory_mb": 0, "inbound_ack_mode": "after_persist"}
        web_config = {**common, "listeners": [{"type": "tcp", "port": port}], "webmail_node_id": 1,
                      "smtp_daily_send_limit": 1, "external_delivery_enabled": False,
                      "persist_min_available_memory_mb": 0}
        for name, config in (("smtp", smtp_config), ("web", web_config)):
            (temp / f"{name}.json").write_text(json.dumps(config))
        commands = [[str(build_dir / "smtpsServer"), str(temp / "smtp.json")],
                    [str(build_dir / "webmailServer"), str(temp / "web.json"), str(port), str(temp / "public")]]
        (temp / "web-second.json").write_text(json.dumps({**web_config, "webmail_node_id": 2}))
        commands.append([str(build_dir / "webmailServer"), str(temp / "web-second.json"), str(second_port), str(temp / "public")])
        (temp / "public").mkdir()
        for index, command in enumerate(commands):
            log = (temp / f"server-{index}.log").open("w+")
            logs.append(log)
            processes.append(subprocess.Popen(command, cwd=temp, stdout=log, stderr=subprocess.STDOUT))
        wait_ready(smtp_ready)
        wait_ready(lambda: api("/api/health")[0] == 200)
        wait_ready(lambda: api("/api/health", node=second_port)[0] == 200)
        payload = {"to": [alias], "cc": [addresses[2], addresses[1]],
                   "subject": "中文主题往返 " * 12, "text": "hello\n.\nlast",
                   "html": "<script>bad()</script><b>ok</b>"}
        assert api("/api/send", payload)[0] == 401
        assert api("/api/login", {"address": addresses[0], "password": password})[0] == 200
        cross_status, cross = api("/api/me", node=second_port)
        assert cross_status == 200, ("cross-node session must be valid", cross_status, cross)
        assert api("/api/send", {**payload, "from": "spoof@webmail.test"})[0] == 400
        assert api("/api/send", {**payload, "bcc": [addresses[2]]})[0] == 400
        assert api("/api/send", payload, content_type="text/plain")[0] == 415
        # A new login revokes the old cookie on every node.
        second_client, _ = new_client()
        assert api("/api/login", {"address": addresses[0], "password": password}, client=second_client, node=second_port)[0] == 200
        old_status, old_session = api("/api/me")
        assert old_status == 401, ("new login must revoke old cookie", old_status, old_session)
        assert api("/api/me", node=second_port)[0] == 401
        assert api("/api/mailboxes")[0] == 401
        assert api("/api/send", payload)[0] == 401
        assert api("/api/logout", {})[0] == 200 # logging out the revoked cookie preserves the new login
        assert api("/api/me", client=second_client)[0] == 200
        assert api("/api/login", {"address": addresses[0], "password": password + "wrong"})[0] == 401
        assert api("/api/me", client=second_client, node=second_port)[0] == 200
        assert api("/api/login", {"address": addresses[0], "password": password})[0] == 200
        assert api("/api/me", client=second_client, node=second_port)[0] == 401
        assert api("/api/logout", {}, client=second_client)[0] == 200
        assert api("/api/me", node=second_port)[0] == 200
        # Concurrent logins across nodes serialize on the unique account key.
        contenders = [new_client() for _ in range(8)]
        def concurrent_login(index):
            client, _ = contenders[index]
            return api("/api/login", {"address": addresses[0], "password": password}, client=client,
                       node=port if index % 2 else second_port)[0]
        with ThreadPoolExecutor(max_workers=8) as executor:
            statuses = list(executor.map(concurrent_login, range(len(contenders))))
        assert statuses == [200] * len(contenders), statuses
        surviving = [(client, jar) for client, jar in contenders if api("/api/me", client=client)[0] == 200]
        assert len(surviving) == 1, ("exactly one concurrent login must survive", len(surviving))
        assert api("/api/me")[0] == 401 # previous sequential login was also replaced
        assert api("/api/logout", {})[0] == 200
        opener, cookie_jar = surviving[0]
        for client, _ in contenders:
            if client is not opener:
                assert api("/api/me", client=client, node=second_port)[0] == 401
                assert api("/api/logout", {}, client=client)[0] == 200
        assert api("/api/me", node=second_port)[0] == 200
        with db.cursor() as cursor:
            cursor.execute("SELECT COUNT(*) FROM web_sessions WHERE account_shard=0 AND user_id=(SELECT id FROM users WHERE mail_address=%s)", (addresses[0],))
            assert cursor.fetchone()[0] == 1

        # Restart a node: session continuity requires the DB rather than any local cache.
        processes[-1].terminate()
        processes[-1].wait(timeout=10)
        processes[-1] = subprocess.Popen(commands[-1], cwd=temp, stdout=logs[-1], stderr=subprocess.STDOUT)
        wait_ready(lambda: api("/api/me", node=second_port)[0] == 200)
        status, sent = api("/api/send", payload, node=second_port)
        assert status == 202 and sent["status"] == "accepted", (status, sent)
        status, denied = api("/api/send", payload)
        assert status == 422 and denied["smtp_code"] == 550 and denied["stage"] == "mail", (status, denied)
        # SMTP and direct HTTP must charge the same account quota, including a forged MAIL FROM.
        with smtplib.SMTP("127.0.0.1", smtp_port, timeout=5) as smtp:
            smtp.login(addresses[0], password)
            code, _ = smtp.mail("spoof@webmail.test")
            assert code == 550, code
        with db.cursor() as cursor:
            cursor.execute("SELECT sent_today FROM users WHERE mail_address=%s", (addresses[0],))
            assert cursor.fetchone()[0] == 1
            cursor.execute("SELECT recipient FROM mail_recipients WHERE sender=%s", (addresses[0],))
            assert sorted(row[0] for row in cursor.fetchall()) == sorted(addresses[1:])
        # 当前提交服务不自动保存 Sent（传统客户端通常另用 IMAP APPEND）。
        for address in addresses[1:]:
            reader, _ = new_client()
            assert api("/api/login", {"address": address, "password": password}, client=reader)[0] == 200
            status, boxes = api("/api/mailboxes", client=reader)
            assert status == 200
            box = next(b for b in boxes["mailboxes"] if b["box_type"] == 1)
            status, page = api(f"/api/mailboxes/{box['id']}/messages", client=reader)
            assert status == 200 and page["total"] == 1, (status, page)
            item = page["messages"][0]
            assert item["subject"] == payload["subject"]
            status, detail = api(f"/api/mailboxes/{box['id']}/messages/{item['id']}", client=reader)
            assert status == 200 and detail["body_available"], (status, detail)
            assert detail["message"]["subject"] == payload["subject"]
            assert detail["body"]["text"] == "hello\r\n.\r\nlast"
            assert detail["body"]["html"] == "<b>ok</b>"
        assert api("/api/me")[0] == 200 # other accounts have independent session slots
        # Cross-node logout, disable, expiry and credential reset all reject old cookies.
        assert api("/api/logout", {}, node=second_port)[0] == 200
        assert api("/api/send", payload)[0] == 401
        assert api("/api/login", {"address": addresses[0], "password": password})[0] == 200
        with db.cursor() as cursor:
            cursor.execute("UPDATE users SET status=0 WHERE mail_address=%s", (addresses[0],))
        assert api("/api/me", node=second_port)[0] == 401
        with db.cursor() as cursor:
            cursor.execute("UPDATE users SET status=1 WHERE mail_address=%s", (addresses[0],))
            cursor.execute("UPDATE web_sessions SET expires_at=UTC_TIMESTAMP()-INTERVAL 1 SECOND WHERE account_address=%s", (addresses[0],))
        assert api("/api/me")[0] == 401
        assert api("/api/login", {"address": addresses[0], "password": password})[0] == 200
        with db.cursor() as cursor:
            cursor.execute("UPDATE users SET password=%s WHERE mail_address=%s", ("changed-for-smoke", addresses[0]))
        assert api("/api/me", node=second_port)[0] == 401
        assert api("/api/login", {"address": addresses[0], "password": password})[0] == 401
        with db.cursor() as cursor:
            cursor.execute("SELECT token_hash,credential_stamp FROM web_sessions WHERE account_address=%s", (addresses[0],))
            records = cursor.fetchall()
            assert records and all(len(t) == 64 and len(c) == 64 for t,c in records)
            token = next(c.value for c in cookie_jar if c.name == "wb_session")
            assert token not in {row[0] for row in records}
            assert hashlib.sha256(token.encode()).hexdigest() in {row[0] for row in records}
        print("PASS: two nodes + restart; single login + 8 concurrent logins; cross-node durable send/read/logout; shared SMTP quota; aliases; disable/expiry/password reset")
    except Exception:
        for index, log in enumerate(logs):
            log.seek(0)
            diagnostic = log.read()[-6000:]
            for secret in (password, hashed, str(db_config.get("password", ""))):
                if secret:
                    diagnostic = diagnostic.replace(secret, "[redacted]")
            print(f"Server {index} diagnostic:\n{diagnostic}")
        raise
    finally:
        for process in reversed(processes):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        for log in logs:
            log.close()
        try:
            with db.cursor() as cursor:
                cursor.execute("SELECT DISTINCT mail_id FROM mail_recipients WHERE sender=%s", (addresses[0],))
                for (mail_id,) in cursor.fetchall():
                    cursor.execute("DELETE FROM mails WHERE id=%s", (mail_id,))
                cursor.execute("DELETE FROM mail_aliases WHERE alias_address=%s", (alias,))
                for address in addresses:
                    cursor.execute("DELETE FROM web_sessions WHERE account_address=%s", (address,))
                    cursor.execute("DELETE FROM users WHERE mail_address=%s", (address,))
        finally:
            db.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build-release"))
    parser.add_argument("--db-config", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="webmail-send-smoke-") as directory:
        temp = Path(directory)
        check_mime(args.build_dir.resolve(), temp)
        if args.db_config:
            check_local_e2e(args.build_dir.resolve(), args.db_config.resolve(), temp)


if __name__ == "__main__":
    main()
