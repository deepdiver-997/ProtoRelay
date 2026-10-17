-- Invite code registration system
-- Run: mysql -u mail_user -p mail < schema.sql

CREATE TABLE IF NOT EXISTS invite_codes (
    id          BIGINT AUTO_INCREMENT PRIMARY KEY,
    code        VARCHAR(64)  NOT NULL UNIQUE,
    max_uses    INT UNSIGNED NOT NULL DEFAULT 5,
    used_count  INT UNSIGNED NOT NULL DEFAULT 0,
    expires_days INT UNSIGNED NOT NULL DEFAULT 90,
    is_active   TINYINT(1)   NOT NULL DEFAULT 1,
    created_at  DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at  DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS invite_registrations (
    id          BIGINT AUTO_INCREMENT PRIMARY KEY,
    user_id     BIGINT NOT NULL,
    code_id     BIGINT NOT NULL,
    invite_code VARCHAR(64)  NOT NULL,
    seq_num     INT UNSIGNED NOT NULL COMMENT '第几个用此邀请码注册的',
    email       VARCHAR(255) NOT NULL,
    expires_at  DATETIME     NOT NULL COMMENT '账号过期时间',
    created_at  DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,
    FOREIGN KEY (code_id) REFERENCES invite_codes(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 邮件别名/组表（2026-10-18 管理后台扩展；与 config/sql/migration_mail_aliases.sql 同构）
CREATE TABLE IF NOT EXISTS mail_aliases (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    alias_address VARCHAR(255) NOT NULL COMMENT '别名/组地址（完整地址，本地域名）',
    target_address VARCHAR(255) NOT NULL COMMENT '投递目标（必须是 users.mail_address 且 status=1）',
    is_active TINYINT(1) NOT NULL DEFAULT 1 COMMENT '0=停用（RCPT 展开跳过）',
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    UNIQUE KEY uk_alias_target (alias_address, target_address),
    INDEX idx_alias_address (alias_address)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
