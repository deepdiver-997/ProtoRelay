-- Webmail single-login policy. Run on authoritative shard 0 after stopping old Webmail writers.
-- Retain the newest existing session per account, breaking timestamp ties by token hash.
-- Repeating this migration is safe. This does not change users or mail data.
DELETE older FROM web_sessions AS older
JOIN web_sessions AS newer
  ON newer.account_shard=older.account_shard AND newer.user_id=older.user_id
 AND (newer.created_at>older.created_at OR
      (newer.created_at=older.created_at AND newer.token_hash>older.token_hash));

SET @webmail_account_key_exists = (
  SELECT COUNT(*) FROM information_schema.STATISTICS
  WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='web_sessions'
    AND INDEX_NAME='uk_web_sessions_account' AND NON_UNIQUE=0
);
SET @webmail_old_key_exists = (
  SELECT COUNT(*) FROM information_schema.STATISTICS
  WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='web_sessions'
    AND INDEX_NAME='idx_web_sessions_user'
);
SET @webmail_single_login_ddl = IF(@webmail_account_key_exists=2, 'SELECT 1',
  CONCAT('ALTER TABLE web_sessions ',
    IF(@webmail_old_key_exists>0, 'DROP INDEX idx_web_sessions_user, ', ''),
    'ADD UNIQUE KEY uk_web_sessions_account (account_shard,user_id)'));
PREPARE webmail_single_login_upgrade FROM @webmail_single_login_ddl;
EXECUTE webmail_single_login_upgrade;
DEALLOCATE PREPARE webmail_single_login_upgrade;
