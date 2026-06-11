#!/usr/bin/env python3
"""
清理过期邀请码账号及其所有关联数据。
用法: python3 cleanup.py [--dry-run]
建议加到 crontab: 0 3 * * 1 cd /opt/smtpServer/register-service && python3 cleanup.py
"""

import json, os, sys, shutil
import mysql.connector

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(BASE_DIR, "config.json")) as f:
    CONFIG = json.load(f)

DB_CONFIG = {
    "host": CONFIG["db_host"],
    "user": CONFIG["db_user"],
    "password": CONFIG["db_password"],
    "database": CONFIG["db_database"],
    "charset": "utf8mb4",
}

# 邮件和附件存储路径（相对 smtpsConfig 工作目录）
MAIL_STORAGE = "/opt/smtpServer/mail"
ATTACHMENT_STORAGE = "/opt/smtpServer/attachments"

DRY_RUN = "--dry-run" in sys.argv


def cleanup():
    db = mysql.connector.connect(**DB_CONFIG)
    cur = db.cursor(dictionary=True)

    # 1. 找出所有过期的邀请码注册用户
    cur.execute("""
        SELECT ir.user_id, ir.email, ir.seq_num, ir.invite_code, ir.expires_at,
               u.mail_address
        FROM invite_registrations ir
        JOIN users u ON u.id = ir.user_id
        WHERE ir.expires_at < NOW()
    """)
    expired = cur.fetchall()

    if not expired:
        print("No expired accounts found.")
        db.close()
        return

    print(f"Found {len(expired)} expired account(s):")
    for r in expired:
        print(f"  {r['mail_address']} (expired {r['expires_at']})")

    if DRY_RUN:
        print("\n[Dry-run] No changes made.")
        db.close()
        return

    expired_ids = [r["user_id"] for r in expired]

    # 2. 收集要删除的邮件文件路径
    cur.execute(
        "SELECT file_path FROM mails m "
        "JOIN mail_recipients mr ON mr.mail_id = m.id "
        "WHERE mr.user_id IN ({})".format(",".join(["%s"] * len(expired_ids))),
        expired_ids)
    mail_files = [row["file_path"] for row in cur.fetchall() if row["file_path"]]

    # 收集附件文件路径
    cur.execute(
        "SELECT a.file_path FROM attachments a "
        "JOIN mails m ON m.id = a.mail_id "
        "JOIN mail_recipients mr ON mr.mail_id = m.id "
        "WHERE mr.user_id IN ({})".format(",".join(["%s"] * len(expired_ids))),
        expired_ids)
    attach_files = [row["file_path"] for row in cur.fetchall() if row["file_path"]]

    # 3. 删除数据库记录（遵守外键约束顺序）
    placeholders = ",".join(["%s"] * len(expired_ids))
    tables = [
        "mail_recipients",        # 收件人关联
        "mail_mailbox",           # 邮件-邮箱关联
        "mail_outbox",            # 发件箱
        "mails",                  # 邮件主表 (前两个依赖它)
        "mailboxes",              # 用户邮箱文件夹
        "attachments",            # 附件
        "invite_registrations",   # 邀请码注册记录
    ]
    for table in tables:
        cur.execute(
            f"DELETE FROM {table} WHERE user_id IN ({placeholders})",
            expired_ids)
        print(f"  Deleted {cur.rowcount} rows from {table}")

    # 删用户
    cur.execute(
        f"DELETE FROM users WHERE id IN ({placeholders})",
        expired_ids)
    print(f"  Deleted {cur.rowcount} user(s)")

    db.commit()

    # 4. 删除磁盘上的文件和空目录
    for fpath in mail_files:
        full = os.path.join(MAIL_STORAGE, fpath) if not os.path.isabs(fpath) else fpath
        if os.path.exists(full):
            os.remove(full)
            print(f"  Removed mail file: {full}")

    for fpath in attach_files:
        full = os.path.join(ATTACHMENT_STORAGE, fpath) if not os.path.isabs(fpath) else fpath
        if os.path.exists(full):
            os.remove(full)
            print(f"  Removed attachment: {full}")

    # 清理空目录
    def prune_empty_dirs(root):
        for dirpath, dirs, files in os.walk(root, topdown=False):
            if dirpath == root:
                continue
            if not os.listdir(dirpath):
                os.rmdir(dirpath)

    prune_empty_dirs(MAIL_STORAGE)
    prune_empty_dirs(ATTACHMENT_STORAGE)

    print(f"\nCleanup complete: {len(expired)} account(s) removed.")

    db.close()


if __name__ == "__main__":
    cleanup()
