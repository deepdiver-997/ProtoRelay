-- 邮件别名/组表（2026-10-18，webmail 开工第一步）
--
-- alias→target 多行 = 免费邮件组：RCPT TO 别名时展开为全部在用目标
-- （展开点 TraditionalSmtpsFsm::resolve_recipient_async，查询
--   db::sql::build_alias_targets_query：
--     SELECT ua.target_address FROM mail_aliases ua
--     JOIN users u ON u.mail_address = ua.target_address AND u.status = 1
--     WHERE ua.alias_address = ? AND ua.is_active = 1）
--
-- 防链设计（先禁 alias→alias）：目标必须是 users 表在用账号。写入侧（管理
-- 后台）校验；读取侧 JOIN users 兜底——别名不是 users 行，即使脏数据写入
-- alias→alias 行，JOIN 也查不出目标，读路径天然无环。
--
-- 全新表，CREATE TABLE IF NOT EXISTS 幂等，可重复执行。

CREATE TABLE IF NOT EXISTS mail_aliases (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    alias_address VARCHAR(255) NOT NULL COMMENT '别名/组地址（完整地址，本地域名）',
    target_address VARCHAR(255) NOT NULL COMMENT '投递目标（必须是 users.mail_address 且 status=1）',
    is_active TINYINT(1) NOT NULL DEFAULT 1 COMMENT '0=停用（RCPT 展开跳过）',
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    UNIQUE KEY uk_alias_target (alias_address, target_address),
    INDEX idx_alias_address (alias_address)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='邮件别名/组表：alias→target 多行=邮件组';
