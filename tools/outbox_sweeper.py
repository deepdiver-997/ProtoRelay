#!/usr/bin/env python3
"""RackNerd 出站中继 outbox 清理器（部署专用，勿并入核心投递逻辑）。

背景：中继(出站边缘 MTA)的 mail_outbox 只存投递元数据，权威副本在阿里云。投递器在
完成时只 mark_sent/mark_dead(更新 status)，不删行、不删正文文件 → 长期运行会不断积累
磁盘(慢烧)。本脚本定时扫：把「全部收件人行都已终态(SENT/DEAD) 且超保留期」的 mail_id
的行删掉，并连带删其正文文件。与权威盒无交集，纯外部运维。

用法：
  python3 outbox_sweeper.py [--db-config /opt/smtpServer/config/db_config.json]
                            [--body-dir /opt/smtpServer/config/mail]
                            [--retention-days 7] [--per-run-max 500]
                            [--dry-run] [--verbose]

退出码：0 成功；1 配置/连接失败；2 执行期错误。
建议 cron：每 6 小时跑一次。
"""
import argparse
import json
import os
import sys
import time
from datetime import datetime, timedelta

STATUS_SENT, STATUS_DEAD = 2, 4          # 见 create_tables.sql: 0-PENDING 1-SENDING 2-SENT 3-RETRY 4-DEAD


def load_db_config(path):
    with open(path) as f:
        return json.load(f)


def connect(cfg):
    try:
        import pymysql
    except ImportError:
        sys.stderr.write("[fatal] 需要 pymysql：pip3 install pymysql（或改用系统 mariadb 客户端+cnf）\n")
        raise
    return pymysql.connect(
        host=cfg.get("host", "127.0.0.1"),
        port=int(cfg.get("port", 3306)),
        user=cfg["user"],
        password=cfg["password"],
        database=cfg["database"],
        charset="utf8mb4",
        autocommit=True,
    )


def build_query(limit_ts, cap):
    # 只选「该 mail_id 所有收件人行都已终态 且 最晚变更早于 limit_ts」的 mail_id。
    # COALESCE(sent_at, updated_at)：SENT 用投递时刻，DEAD 用最近更新。
    return """
      SELECT mail_id, COUNT(*) AS n, COALESCE(MAX(sent_at), MAX(updated_at)) AS last_t
      FROM mail_outbox
      WHERE status IN ({sent},{dead})
        AND (sent_at IS NOT NULL OR updated_at < %(limit)s)
      GROUP BY mail_id
      HAVING COUNT(*) = SUM(status IN ({sent},{dead}))
         AND MAX(COALESCE(sent_at, updated_at)) < %(limit)s
      ORDER BY last_t ASC
      LIMIT %(cap)s
    """.format(sent=STATUS_SENT, dead=STATUS_DEAD)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--db-config", default="/opt/smtpServer/config/db_config.json")
    ap.add_argument("--body-dir", default="/opt/smtpServer/config/mail")
    ap.add_argument("--retention-days", type=int, default=7)
    ap.add_argument("--per-run-max", type=int, default=500)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.db_config):
        sys.stderr.write(f"[fatal] db_config 不存在: {args.db_config}\n")
        return 1

    limit_ts = datetime.now() - timedelta(days=args.retention_days)
    try:
        conn = connect(load_db_config(args.db_config))
    except Exception as e:
        sys.stderr.write(f"[fatal] DB 连接失败: {e}\n")
        return 1

    deleted_rows = 0
    freed_bodies = 0
    removed_mail_ids = []
    try:
        with conn.cursor() as cur:
            cur.execute(build_query(limit_ts, args.per_run_max))
            rows = cur.fetchall()
            mail_ids = [r[0] for r in rows]
            if args.verbose:
                for r in rows:
                    print(f"  candidate mail_id={r[0]} rows={r[1]} last={r[2]}")

            if not mail_ids:
                print(f"outbox_sweeper: 无待清理(mail_id ≤ {args.per_run_max} 已达终态且 < {args.retention_days}d)")
                return 0

            placeholders = ",".join(["%s"] * len(mail_ids))
            del_sql = (f"DELETE FROM mail_outbox WHERE mail_id IN ({placeholders}) "
                       f"AND status IN ({STATUS_SENT},{STATUS_DEAD})")
            if args.dry_run:
                print("[dry-run] 拟删除 sql:", del_sql % tuple(mail_ids))
                # dry-run 下预估行数
                cur.execute(f"SELECT COUNT(*) FROM mail_outbox WHERE mail_id IN ({placeholders}) "
                            f"AND status IN ({STATUS_SENT},{STATUS_DEAD})", tuple(mail_ids))
                deleted_rows = cur.fetchone()[0]
            else:
                deleted_rows = cur.rowcount
                cur.execute(del_sql, tuple(mail_ids))
                deleted_rows = cur.rowcount

        # 正文文件：仅删已确认整体删除的 mail_id 对应文件
        if not args.dry_run:
            for mid in mail_ids:
                p = os.path.join(args.body_dir, str(mid))
                if os.path.isfile(p):
                    try:
                        os.remove(p)
                        freed_bodies += 1
                        removed_mail_ids.append(mid)
                    except OSError as e:
                        print(f"  warn: 删正文失败 {p}: {e}")
            size = sum(os.path.getsize(os.path.join(args.body_dir, str(m)))
                       for m in mail_ids
                       if os.path.exists(os.path.join(args.body_dir, str(m))))
        else:
            for m in mail_ids:
                p = os.path.join(args.body_dir, str(m))
                if os.path.isfile(p):
                    freed_bodies += 1
                    removed_mail_ids.append(m)
                    if args.verbose:
                        print(f"  [dry-run] 将删正文 {p} ({os.path.getsize(p)}B)")
            size = 0
    except Exception as e:
        sys.stderr.write(f"[error] 清理执行失败: {e}\n")
        conn.close()
        return 2
    finally:
        conn.close()

    print(f"outbox_sweeper: 删行={deleted_rows} 删正文={freed_bodies} mail_id={len(mail_ids)}")
    if removed_mail_ids and args.verbose:
        print("  removed:", ",".join(map(str, removed_mail_ids)))
    return 0


if __name__ == "__main__":
    sys.exit(main())