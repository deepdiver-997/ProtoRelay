#!/usr/bin/env python3
"""Local DB regression: migrate legacy multi-session rows, retain one per account, rerun safely.

.venv/bin/python test/scripts/webmail_single_login_migration_test.py --db-config config/db_config.json
Only creates/removes a randomly named test table, without changing actual sessions/users/mail.
"""
import argparse
import json
from pathlib import Path
import secrets
import mysql.connector


def run_script(cursor, sql):
    for statement in sql.split(";"):
        if statement.strip():
            cursor.execute(statement)
            if cursor.with_rows:
                cursor.fetchall()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db-config", type=Path, required=True)
    args = parser.parse_args()
    config = json.loads(args.db_config.read_text())
    if config.get("host") not in ("localhost", "127.0.0.1", "::1"):
        raise ValueError("This migration test only supports a local database")
    db = mysql.connector.connect(**{k: config[k] for k in ("host", "port", "user", "password", "database") if k in config}, autocommit=True)
    table = "web_sessions_migration_" + secrets.token_hex(8)
    migration = (Path(__file__).resolve().parents[2] / "config/sql/migration_web_sessions_single_login.sql").read_text().replace("web_sessions", table)
    try:
        with db.cursor() as cursor:
            cursor.execute(f"CREATE TABLE {table} (token_hash CHAR(64) CHARACTER SET ascii COLLATE ascii_bin PRIMARY KEY,user_id BIGINT UNSIGNED NOT NULL,account_shard INT UNSIGNED NOT NULL,created_at DATETIME NOT NULL,KEY idx_web_sessions_user(account_shard,user_id)) ENGINE=InnoDB")
            # Migration also contains index/variable identifiers: keep the actual legacy index name.
            migration = migration.replace("idx_" + table + "_user", "idx_web_sessions_user").replace("uk_" + table + "_account", "uk_web_sessions_account")
            for token, user, shard, created in [("0"*64,42,0,"2026-01-01"),("1"*64,42,0,"2026-01-02"),("2"*64,42,0,"2026-01-02"),("3"*64,42,1,"2026-01-01"),("4"*64,43,0,"2026-01-01")]:
                cursor.execute(f"INSERT INTO {table} VALUES (%s,%s,%s,%s)", (token,user,shard,created))
            run_script(cursor, migration)
            run_script(cursor, migration)
            cursor.execute(f"SELECT token_hash FROM {table}")
            assert {r[0] for r in cursor.fetchall()} == {"2"*64,"3"*64,"4"*64}
            try:
                cursor.execute(f"INSERT INTO {table} VALUES (%s,42,0,'2026-01-03')", ("5"*64,))
            except mysql.connector.IntegrityError:
                pass
            else:
                raise AssertionError("unique account constraint did not reject a second row")
        print("PASS: legacy rows deduplicated by account/shard; deterministic tie; unique key; idempotent migration")
    finally:
        with db.cursor() as cursor:
            cursor.execute(f"DROP TABLE IF EXISTS {table}")
        db.close()


if __name__ == "__main__":
    main()
