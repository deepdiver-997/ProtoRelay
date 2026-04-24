#!/usr/bin/env bash

set -euo pipefail

HOST="127.0.0.1"
PORT="3306"
USER="root"
PASSWORD=""
DB_NAME="mail"
DB_PROVIDED="0"
MODE="mail_related"

MAIL_TABLES=(
  "mail_outbox"
  "attachments"
  "mail_recipients"
  "mails"
)

print_usage() {
  cat <<'EOF'
Usage:
  ./scripts/clear_mysql_mail_data.sh [options]

Default behavior:
  - Connect to local MySQL with root and no password
  - Target database: mail
  - Clear mail-related tables only: mails, mail_recipients, attachments, mail_outbox

If you explicitly pass --database/-d, the script switches to all-table cleanup mode
for that database (drops all tables in that database after confirmation).

Options:
  -h, --host <host>         MySQL host (default: 127.0.0.1)
  -P, --port <port>         MySQL port (default: 3306)
  -u, --user <user>         MySQL user (default: root)
  -p, --password <pass>     MySQL password (default: empty)
  -d, --database <name>     Database name; when provided, delete all tables in it
      --all-tables          Force all-table cleanup mode
      --mail-only           Force mail-related-table cleanup mode
      --help                Show this help message

Examples:
  ./scripts/clear_mysql_mail_data.sh
  ./scripts/clear_mysql_mail_data.sh -u abc -p 123456 -d abc
  ./scripts/clear_mysql_mail_data.sh -d abc --all-tables
  ./scripts/clear_mysql_mail_data.sh -d mail --mail-only
EOF
}

require_arg() {
  local flag="$1"
  local value="${2:-}"
  if [[ -z "$value" || "$value" == -* ]]; then
    echo "Error: $flag requires a value"
    exit 1
  fi
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--host)
      require_arg "$1" "${2:-}"
      HOST="$2"
      shift 2
      ;;
    -P|--port)
      require_arg "$1" "${2:-}"
      PORT="$2"
      shift 2
      ;;
    -u|--user)
      require_arg "$1" "${2:-}"
      USER="$2"
      shift 2
      ;;
    -p|--password)
      require_arg "$1" "${2:-}"
      PASSWORD="$2"
      shift 2
      ;;
    -d|--database)
      require_arg "$1" "${2:-}"
      DB_NAME="$2"
      DB_PROVIDED="1"
      MODE="all_tables"
      shift 2
      ;;
    --all-tables)
      MODE="all_tables"
      shift
      ;;
    --mail-only)
      MODE="mail_related"
      shift
      ;;
    --help)
      print_usage
      exit 0
      ;;
    *)
      echo "Error: Unknown argument: $1"
      print_usage
      exit 1
      ;;
  esac
done

if ! command -v mysql >/dev/null 2>&1; then
  echo "Error: mysql client not found in PATH"
  exit 1
fi

if [[ ! "$PORT" =~ ^[0-9]+$ ]]; then
  echo "Error: invalid port: $PORT"
  exit 1
fi

if [[ ! "$DB_NAME" =~ ^[A-Za-z0-9_]+$ ]]; then
  echo "Error: invalid database name: $DB_NAME"
  exit 1
fi

MYSQL_AUTH=( -h "$HOST" -P "$PORT" -u "$USER" )
if [[ -n "$PASSWORD" ]]; then
  MYSQL_AUTH+=( -p"$PASSWORD" )
fi

if ! mysql "${MYSQL_AUTH[@]}" -Nse "SELECT 1;" >/dev/null 2>&1; then
  echo "Error: failed to connect to MySQL at ${HOST}:${PORT} as ${USER}"
  exit 1
fi

DB_EXISTS=$(mysql "${MYSQL_AUTH[@]}" -Nse "SELECT COUNT(*) FROM information_schema.schemata WHERE schema_name='${DB_NAME}';")
if [[ "$DB_EXISTS" != "1" ]]; then
  echo "Error: database does not exist: ${DB_NAME}"
  exit 1
fi

if [[ "$MODE" == "all_tables" ]]; then
  TABLE_COUNT=$(mysql "${MYSQL_AUTH[@]}" -Nse "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='${DB_NAME}';")

  echo "Target database: ${DB_NAME}"
  echo "Action: delete ALL tables in this database"
  echo "Host/User: ${HOST}:${PORT} / ${USER}"
  echo "Table count: ${TABLE_COUNT}"
  echo

  read -r -p "Type YES to continue deleting all tables in ${DB_NAME}: " CONFIRM
  if [[ "$CONFIRM" != "YES" ]]; then
    echo "Cancelled."
    exit 0
  fi

  if [[ "$TABLE_COUNT" == "0" ]]; then
    echo "No tables found in ${DB_NAME}. Nothing to do."
    exit 0
  fi

  {
    echo "SET FOREIGN_KEY_CHECKS=0;"
    mysql "${MYSQL_AUTH[@]}" -Nse "SELECT CONCAT('DROP TABLE IF EXISTS \\\`', table_name, '\\\`;') FROM information_schema.tables WHERE table_schema='${DB_NAME}';"
    echo "SET FOREIGN_KEY_CHECKS=1;"
  } | mysql "${MYSQL_AUTH[@]}" "${DB_NAME}"

  echo "Done. All tables in ${DB_NAME} were deleted."
  exit 0
fi

EXISTING_TABLES=()
for t in "${MAIL_TABLES[@]}"; do
  EXISTS=$(mysql "${MYSQL_AUTH[@]}" -Nse "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='${DB_NAME}' AND table_name='${t}';")
  if [[ "$EXISTS" == "1" ]]; then
    EXISTING_TABLES+=( "$t" )
  fi
done

if [[ ${#EXISTING_TABLES[@]} -eq 0 ]]; then
  echo "No mail-related tables found in ${DB_NAME}. Nothing to do."
  exit 0
fi

echo "Target database: ${DB_NAME}"
echo "Action: clear mail-related tables"
echo "Host/User: ${HOST}:${PORT} / ${USER}"
echo "Tables: ${EXISTING_TABLES[*]}"
echo
read -r -p "Type YES to continue clearing mail-related data: " CONFIRM
if [[ "$CONFIRM" != "YES" ]]; then
  echo "Cancelled."
  exit 0
fi

for t in "${EXISTING_TABLES[@]}"; do
  if ! mysql "${MYSQL_AUTH[@]}" "${DB_NAME}" -e "SET FOREIGN_KEY_CHECKS=0; TRUNCATE TABLE \\`${t}\\`; SET FOREIGN_KEY_CHECKS=1;"; then
    echo "Warn: TRUNCATE failed for ${t}, fallback to DELETE"
    mysql "${MYSQL_AUTH[@]}" "${DB_NAME}" -e "SET FOREIGN_KEY_CHECKS=0; DELETE FROM \\`${t}\\`; SET FOREIGN_KEY_CHECKS=1;"
  fi
done

echo "Done. Mail-related data has been cleared from ${DB_NAME}."
