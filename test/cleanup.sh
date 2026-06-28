#!/bin/bash
# 清理测试数据 — mail 目录、DB、tmp 文件
# 用法: ./cleanup.sh [--all]

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

echo "=== Cleanup ==="

# 1. 邮件落盘文件（rm -rf 比 find -delete 快 100x，避免 ARG_MAX）
echo -n "Mail files... "
rm -rf "$PROJECT_DIR/mail" && mkdir -p "$PROJECT_DIR/mail"
rm -rf "$PROJECT_DIR/attachments" && mkdir -p "$PROJECT_DIR/attachments"
echo "done"

# 2. 临时目录
rm -rf /tmp/fsm_bench_mail /tmp/fsm_bench_att 2>/dev/null

# 3. 数据库（需要 mysql 可用）
if [ "${1:-}" = "--all" ] && command -v mysql &>/dev/null; then
    echo -n "Database... "
    for db in mail_system mail_system_dev mail_system_test; do
        mysql -u root -e "
          DELETE FROM mail_recipients; DELETE FROM mails;
          DELETE FROM attachments; DELETE FROM mail_mailbox;
          DELETE FROM mail_outbox;
        " "$db" 2>/dev/null || true
    done
    echo "done"
fi

# 4. 进程
pkill -9 -f smtpsServer 2>/dev/null || true
pkill -9 -f imapsServer 2>/dev/null || true
pkill -9 -f fsm_bench 2>/dev/null || true

echo "=== Cleanup complete ==="
echo "  mail/attachments dirs reset"
[ "${1:-}" = "--all" ] && echo "  DB tables truncated" || echo "  (use --all to also clean DB)"
echo "  server processes killed"
