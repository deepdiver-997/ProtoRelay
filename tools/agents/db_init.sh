#!/bin/bash
# tools/agents/db_init.sh — 为一个 agent worktree 建独立 MySQL 库并建表（或删掉它）
#
# 用法：
#   db_init.sh --db-config <worktree>/config/db_config.json [--sql <create_tables.sql>]
#   db_init.sh --db-config <...> --drop [--yes]
#
# 连接参数（host/port/user/password/database）全部来自 db_config.json —— 也就是
# apply_agent_env.py 已把 database 改成 mail_agent_N 的那份副本。密码只通过 MYSQL_PWD 环境
# 传给 mysql 客户端。建表只跑 create_tables.sql（它已是完整当前 schema；migration_*.sql 是
# 给老库升级用的，新库跑会撞列）。脚本里即使残留 USE / CREATE DATABASE 也会被滤掉，
# 表一定建在 db_config.json 指定的库里（服务端 MySQLPool 启动时同样如此）。
#
# 保护：库名不匹配 ${DB_NAME_PREFIX}<数字>（默认 mail_agent_N）时拒绝 --drop，也拒绝对共享库
# 建表；这样永远不会误伤 hub 的 `mail` 库。

set -e
# shellcheck disable=SC1091
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"
agents_init

DB_CONFIG=""
SQL_FILE=""
DROP=0
YES=0
while [ $# -gt 0 ]; do
    case "$1" in
        --db-config) DB_CONFIG="$2"; shift 2 ;;
        --sql)       SQL_FILE="$2"; shift 2 ;;
        --drop)      DROP=1; shift ;;
        --yes|-y)    YES=1; shift ;;
        -h|--help)   sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "未知参数：$1" ;;
    esac
done
[ -n "${DB_CONFIG}" ] || die "需要 --db-config <db_config.json>"
[ -f "${DB_CONFIG}" ] || die "文件不存在：${DB_CONFIG}"
command -v mysql >/dev/null 2>&1 || die "找不到 mysql 客户端（brew install mysql-client）"

DB_HOST="$(json_get "${DB_CONFIG}" host)"; DB_HOST="${DB_HOST:-localhost}"
DB_PORT="$(json_get "${DB_CONFIG}" port)"; DB_PORT="${DB_PORT:-3306}"
DB_USER="$(json_get "${DB_CONFIG}" user)"
DB_PASS="$(json_get "${DB_CONFIG}" password)"
DB_NAME="$(json_get "${DB_CONFIG}" database)"
[ -n "${DB_USER}" ] && [ -n "${DB_NAME}" ] || die "db_config.json 缺 user/database"

if ! [[ "${DB_NAME}" =~ ^${DB_NAME_PREFIX}[0-9]+$ ]]; then
    die "库名 '${DB_NAME}' 不是 agent 专用库（应为 ${DB_NAME_PREFIX}<N>）——拒绝操作，避免误伤共享库"
fi

export MYSQL_PWD="${DB_PASS}"
MYSQL=(mysql -h "${DB_HOST}" -P "${DB_PORT}" -u "${DB_USER}")

if [ "${DROP}" = "1" ]; then
    if [ "${YES}" != "1" ]; then
        printf '确认删除数据库 %s@%s:%s ？[y/N] ' "${DB_NAME}" "${DB_HOST}" "${DB_PORT}"
        read -r ans
        [ "${ans}" = "y" ] || [ "${ans}" = "Y" ] || die "已取消"
    fi
    "${MYSQL[@]}" -e "DROP DATABASE IF EXISTS \`${DB_NAME}\`;"
    ok "已删除数据库 ${DB_NAME}"
    exit 0
fi

if [ -z "${SQL_FILE}" ]; then
    SQL_FILE="$(cd "$(dirname "${DB_CONFIG}")" && pwd)/sql/create_tables.sql"
fi
[ -f "${SQL_FILE}" ] || die "建表脚本不存在：${SQL_FILE}"

log "连接 ${DB_USER}@${DB_HOST}:${DB_PORT}，建库 ${DB_NAME}"
if ! "${MYSQL[@]}" -e "CREATE DATABASE IF NOT EXISTS \`${DB_NAME}\` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;"; then
    die "建库失败：请确认 MySQL 已启动，且用户 ${DB_USER} 有 CREATE 权限（或由管理员执行：CREATE DATABASE ${DB_NAME}; GRANT ALL ON ${DB_NAME}.* TO '${DB_USER}'@'%';）"
fi
log "建表：$(basename "${SQL_FILE}")（滤掉脚本里残留的 CREATE DATABASE / USE，保证建在 ${DB_NAME} 里）"
grep -viE '^[[:space:]]*(CREATE[[:space:]]+DATABASE|USE)[[:space:]]' "${SQL_FILE}" | "${MYSQL[@]}" "${DB_NAME}"
TABLES="$("${MYSQL[@]}" -N -e "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='${DB_NAME}';")"
[ "${TABLES}" -gt 0 ] || die "建表后 ${DB_NAME} 仍为空，请检查 ${SQL_FILE}"
ok "数据库 ${DB_NAME} 就绪（${TABLES} 张表）"
