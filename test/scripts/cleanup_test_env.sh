#!/bin/bash
# 清理测试环境（由 test/scripts/setup_test_env.py 生成，请勿手改）
set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_DIR="${SCRIPT_DIR}/../config"
TMP_ROOT="${PROTORELAY_TEST_TMP_ROOT:-/tmp/protorelay_test}"
rm -rf "${TMP_ROOT}"
rm -rf "${CONFIG_DIR}/crt" "${CONFIG_DIR}/dkim" "${CONFIG_DIR}/sql_init" "${CONFIG_DIR}/sql"
rm -f "${CONFIG_DIR}/create_tables.sql"
rm -f "${CONFIG_DIR}/smtps_mock.json" "${CONFIG_DIR}/smtps_real.json"
rm -f "${CONFIG_DIR}/imaps_mock.json" "${CONFIG_DIR}/imaps_real.json"
rm -f "${CONFIG_DIR}/db_config.json" "${CONFIG_DIR}/router_config.json"
pkill -f "smtpsServer.*test/config" 2>/dev/null || true
pkill -f "imapsServer.*test/config" 2>/dev/null || true
echo "Test environment cleaned."
