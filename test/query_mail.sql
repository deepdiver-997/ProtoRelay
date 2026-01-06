-- 查看最新插入的邮件
SELECT * FROM mails ORDER BY id DESC LIMIT 1;

-- 查看所有邮件
SELECT * FROM mails;

-- 查看表结构
DESCRIBE mails;
