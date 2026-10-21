#!/usr/bin/env python3
"""IMAP 读路径压测的 seed 数据：给测试用户 INBOX 灌 K 封邮件（DB 行 + body 文件）。

读场景不产生新数据，seed 一次即可；重复跑只是重读同一份数据。
用 INSERT IGNORE（按 mail_id 幂等），重复执行不会重复灌。
注意：读路径的唯一写入是 get_mailbox_uidnext 的高水位推进（幂等 GREATEST），
对压测数据无害。

用法: python3 test/bench/imap/seed_imap_data.py [--mails 200] [--size 2048]

数据库连接：默认读 config/db_config.json（host/user/password/database/port），
可用 --db-config 指定别的文件，或用 --host/--user/--database 覆盖；
密码也可用环境变量 MYSQL_PWD 覆盖。密码只通过环境传给 mysql 客户端，不进 argv。
"""
import argparse, json, os, subprocess, sys

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
DEFAULT_DB_CONFIG = os.path.join(PROJECT_DIR, "config", "db_config.json")

DB = []          # mysql 客户端参数，在 main() 里根据配置拼出
DB_ENV = None    # 传给子进程的环境（含 MYSQL_PWD）

def q(sql):
    r = subprocess.run(DB + ["-e", sql], capture_output=True, text=True, env=DB_ENV)
    if r.returncode != 0:
        print("SQL error:", r.stderr, file=sys.stderr)
        sys.exit(1)

def load_db_settings(a):
    cfg = {}
    if os.path.exists(a.db_config):
        with open(a.db_config) as f:
            cfg = json.load(f)
    elif a.db_config != DEFAULT_DB_CONFIG:
        print(f"db config not found: {a.db_config}", file=sys.stderr)
        sys.exit(1)
    host = a.host or cfg.get("host", "localhost")
    user = a.user_db or cfg.get("user")
    database = a.database or cfg.get("database")
    port = str(cfg.get("port", 3306))
    password = os.environ.get("MYSQL_PWD") or cfg.get("password")
    missing = [k for k, v in (("user", user), ("database", database), ("password", password)) if not v]
    if missing:
        print(f"missing DB settings: {', '.join(missing)} "
              f"(provide config/db_config.json via --db-config, or --user-db/--database + MYSQL_PWD)",
              file=sys.stderr)
        sys.exit(1)
    env = dict(os.environ)
    env["MYSQL_PWD"] = password
    return ["mysql", "-h", host, "-P", port, "-u", user, database], env

def main():
    global DB, DB_ENV
    p = argparse.ArgumentParser()
    p.add_argument("--mails", type=int, default=200, help="每用户 INBOX 邮件数")
    p.add_argument("--user", default="test2@scut.email")
    p.add_argument("--mailbox", type=int, default=6, help="收件箱 mailbox id（默认 test2 的）")
    p.add_argument("--body-dir", default="mail", help="body 文件目录（相对项目根）")
    p.add_argument("--size", type=int, default=2048, help="每封 body 字节数")
    p.add_argument("--db-config", default=DEFAULT_DB_CONFIG, help="数据库配置 JSON（默认 config/db_config.json）")
    p.add_argument("--host", default=None, help="覆盖 DB host")
    p.add_argument("--user-db", default=None, help="覆盖 DB 用户名")
    p.add_argument("--database", default=None, help="覆盖 DB 库名")
    a = p.parse_args()
    DB, DB_ENV = load_db_settings(a)

    r = subprocess.run(DB + ["-N", "-e", f"SELECT id FROM users WHERE mail_address='{a.user}'"],
                       capture_output=True, text=True, env=DB_ENV)
    uid = r.stdout.strip()
    if not uid:
        print(f"user {a.user} not found", file=sys.stderr); sys.exit(1)
    uid = int(uid)

    os.makedirs(a.body_dir, exist_ok=True)
    base = 10**18          # mails.id（单调递增，uidnext 友好）
    rbase = 2 * 10**18     # mail_recipients.id（服务端生成式）

    m_rows, r_rows, mm_rows = [], [], []
    for i in range(a.mails):
        mid = base + i
        subject = f"Bench mail {i}"
        body_path = os.path.abspath(os.path.join(a.body_dir, str(mid)))
        m_rows.append(f"({mid}, '{subject}', '{body_path}', NOW())")
        r_rows.append(f"({rbase+i}, {mid}, 'sender@scut.email', '{a.user}', 0)")
        # 收件人 status=0 已读；改 1 可测 UNSEEN 路径
        mm_rows.append(f"({mid}, {a.mailbox}, {uid}, 0, 0, 0, NOW())")
        with open(body_path, "wb") as f:
            f.write((f"Subject: {subject}\r\n\r\n" + ("x" * a.size)).encode())

    q("INSERT IGNORE INTO mails (id, subject, body_path, send_time) VALUES " + ",".join(m_rows))
    q("INSERT IGNORE INTO mail_recipients (id, mail_id, sender, recipient, status) VALUES " + ",".join(r_rows))
    q("INSERT IGNORE INTO mail_mailbox (mail_id, mailbox_id, user_id, is_starred, is_important, is_deleted, add_time) VALUES " + ",".join(mm_rows))
    print(f"seeded {a.mails} mails for {a.user} (mailbox {a.mailbox}), bodies in {a.body_dir}/")

if __name__ == "__main__":
    main()
