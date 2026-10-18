
-- Shared Webmail session registry (authoritative shard 0). No raw tokens/passwords.
CREATE TABLE IF NOT EXISTS web_sessions (
    token_hash CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL PRIMARY KEY,
    user_id BIGINT UNSIGNED NOT NULL,
    account_address VARCHAR(255) NOT NULL,
    account_shard INT UNSIGNED NOT NULL,
    credential_stamp CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    expires_at DATETIME NOT NULL,
    KEY idx_web_sessions_user (account_shard,user_id),
    KEY idx_web_sessions_expiry (expires_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
