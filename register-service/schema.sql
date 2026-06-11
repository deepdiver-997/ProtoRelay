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
