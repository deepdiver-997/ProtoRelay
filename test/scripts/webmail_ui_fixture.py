#!/usr/bin/env python3
"""Real local DB/HTTP fixture for the production Webmail UI (no external delivery).

.venv/bin/python test/scripts/webmail_ui_fixture.py --db-config config/db_config.json
Add --serve --state /tmp/webmail-ui-state.json for manual browser inspection.
All accounts, messages, files and processes created here are removed on exit.
"""
import argparse
from datetime import datetime, timedelta
from email.message import EmailMessage
from email import policy
import hashlib
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import tempfile
import time
import urllib.request
import bcrypt
import mysql.connector

ROOT = Path(__file__).resolve().parents[2]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--db-config', type=Path, required=True)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build-release')
    parser.add_argument('--state', type=Path, default=Path('/tmp/webmail-ui-state.json'))
    parser.add_argument('--serve', action='store_true')
    args = parser.parse_args()
    cfg_path = args.db_config.resolve()
    cfg = json.loads(cfg_path.read_text())
    if cfg.get('host') not in ('localhost', '127.0.0.1', '::1'):
        raise ValueError('Only a local database is supported')
    db = mysql.connector.connect(**{k: cfg[k] for k in ('host', 'port', 'user', 'password', 'database') if k in cfg}, autocommit=True)
    tag = secrets.token_hex(4)
    addresses = [f'{name}.{tag}@webmail.test' for name in ('lin', 'chen', 'zhou')]
    password = secrets.token_urlsafe(20)
    hashed = bcrypt.hashpw(password.encode(), bcrypt.gensalt(rounds=4)).decode()
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0)); port = probe.getsockname()[1]
    process = None
    with tempfile.TemporaryDirectory(prefix='webmail-ui-') as tmp:
        temp = Path(tmp)
        mail_dir = temp / 'mail'; mail_dir.mkdir()
        attachment_dir = temp / 'attachments'; attachment_dir.mkdir()
        attachment = bytes(range(256)) * 4096 + b'\x00webmail-download\xff'
        attachment_path = attachment_dir / 'reference.bin'; attachment_path.write_bytes(attachment)
        ids = []
        try:
            with db.cursor() as cur:
                for address in addresses:
                    cur.execute('INSERT INTO users (mail_address,password,name,status) VALUES (%s,%s,%s,1)', (address, hashed, 'Webmail UI test'))
                cur.execute('SELECT id,mail_address FROM users WHERE mail_address IN (%s,%s,%s)', addresses)
                users = {address: uid for uid, address in cur.fetchall()}
                cur.execute('SELECT user_id,id,box_type FROM mailboxes WHERE user_id IN (%s,%s,%s)', list(users.values()))
                boxes = {(uid, kind): bid for uid, bid, kind in cur.fetchall()}
                base_id = 9007199254741000 + int(secrets.token_hex(5), 16) * 100
                subjects = ['设计评审｜新版邮箱，一起让沟通更简单', '周末小聚 · 一起去公园走走', '十月计划与近期进度', '欢迎使用 ProtoRelay 邮箱', '会议纪要：产品与工程同步']
                for i in range(24):
                    mid = base_id + i; ids.append(mid)
                    subject = subjects[i] if i < len(subjects) else f'项目进展周报 · 第 {24-i} 期'
                    msg = EmailMessage(policy=policy.SMTP)
                    msg['From'] = addresses[1]; msg['To'] = addresses[0]; msg['Cc'] = addresses[2]; msg['Subject'] = subject
                    msg.set_content('你好，\n\n新版邮箱的设计方案已经整理好了。希望阅读和写信都能更加清晰、顺手。\n我们周五下午一起看看，欢迎回复你的想法。\n\n陈知远\n产品与设计', charset='utf-8')
                    msg.add_alternative('<h2>让每一封邮件，都有清晰的来处。</h2><p>你好，</p><p>新版邮箱的设计方案已经整理好了。我们希望，<b>阅读与写信都能更加清晰、顺手</b>，也让重要的消息更容易被找到。</p><p>本次评审主要讨论：</p><ul><li>收件箱的信息层次与阅读体验</li><li>写信流程和草稿保留</li><li>移动设备上的使用细节</li></ul><p>我们周五下午一起看看，欢迎回复你的想法。</p><p>陈知远<br>产品与设计</p><script>parent.__mailScriptExecuted=true</script><img src="https://tracker.invalid/pixel" onerror="parent.__mailScriptExecuted=true">', subtype='html', charset='utf-8')
                    body_path = mail_dir / f'{mid}.eml'; body_path.write_bytes(msg.as_bytes())
                    cur.execute('INSERT INTO mails (id,subject,body_path,send_time) VALUES (%s,%s,%s,%s)', (mid, subject, str(body_path), datetime.now() - timedelta(hours=i)))
                    for j, recipient in enumerate((addresses[0], addresses[2])):
                        cur.execute('INSERT INTO mail_recipients (id,mail_id,sender,recipient,status) VALUES (%s,%s,%s,%s,%s)', (base_id + 100 + i*2+j, mid, addresses[1], recipient, 1 if i < 4 else 0))
                        cur.execute('INSERT INTO mail_mailbox (mail_id,mailbox_id,user_id,is_starred) VALUES (%s,%s,%s,%s)', (mid, boxes[(users[recipient], 1)], users[recipient], i == 2))
                cur.execute('INSERT INTO attachments (mail_id,filename,filepath,file_size,mime_type) VALUES (%s,%s,%s,%s,%s)', (ids[0], '项目资料.bin', str(attachment_path), len(attachment), 'application/octet-stream'))
                attachment_id = cur.lastrowid
            config = {'address': '127.0.0.1', 'system_domain': 'webmail.test', 'use_database': True, 'db_config_file': str(cfg_path), 'io_thread_count': 2, 'worker_thread_count': 4, 'perf_mode': True, 'metrics_enabled': False, 'log_level': 'warn', 'log_to_console': True, 'log_to_file': False, 'intrusion_detection_enabled': False, 'listeners': [{'type': 'tcp', 'port': port}], 'webmail_node_id': 3, 'smtp_daily_send_limit': 100, 'external_delivery_enabled': False, 'persist_min_available_memory_mb': 0, 'storage': {'provider': 'local', 'local': {'mail_path': str(mail_dir), 'attachment_path': str(attachment_dir)}}}
            config_path = temp / 'web.json'; config_path.write_text(json.dumps(config))
            with (temp / 'server.log').open('w+') as log:
                process = subprocess.Popen([str(args.build_dir.resolve() / 'webmailServer'), str(config_path), str(port), str(ROOT / 'build-webmail')], cwd=temp, stdout=log, stderr=subprocess.STDOUT)
                base = f'http://127.0.0.1:{port}'
                for _ in range(200):
                    if process.poll() is not None: raise RuntimeError('Webmail server exited')
                    try:
                        with urllib.request.urlopen(base + '/api/health', timeout=.5) as res:
                            if res.status == 200: break
                    except OSError: time.sleep(.05)
                else: raise RuntimeError('Webmail server did not start')
                state = {'baseURL': base, 'address': addresses[0], 'password': password, 'sender': addresses[1], 'cc': addresses[2], 'mailId': str(ids[0]), 'boxId': str(boxes[(users[addresses[0]],1)]), 'ccBoxId': str(boxes[(users[addresses[2]],1)]), 'attachmentId': str(attachment_id), 'attachmentHash': hashlib.sha256(attachment).hexdigest(), 'attachmentSize': len(attachment)}
                args.state.write_text(json.dumps(state)); args.state.chmod(0o600)
                print(f'UI fixture ready: {base}; isolated data; state={args.state}', flush=True)
                if args.serve:
                    while process.poll() is None: time.sleep(.5)
                else:
                    env = {**os.environ, 'WEBMAIL_E2E_STATE': str(args.state)}
                    result = subprocess.run(['npm', 'run', 'test:e2e'], cwd=ROOT / 'webmail', env=env)
                    if result.returncode:
                        log.seek(0); diagnostic = log.read()[-5000:]
                        for secret in (password, hashed, str(cfg.get('password',''))):
                            if secret: diagnostic = diagnostic.replace(secret, '[redacted]')
                        print(diagnostic)
                        raise SystemExit(result.returncode)
        finally:
            if process and process.poll() is None:
                process.terminate()
                try: process.wait(timeout=10)
                except subprocess.TimeoutExpired: process.kill(); process.wait()
            with db.cursor() as cur:
                # Includes messages sent by the browser tests, all within the fixture accounts.
                cur.execute('SELECT DISTINCT mail_id FROM mail_recipients WHERE sender IN (%s,%s,%s)', addresses)
                for (mid,) in cur.fetchall(): cur.execute('DELETE FROM mails WHERE id=%s', (mid,))
                for address in addresses:
                    cur.execute('DELETE FROM web_sessions WHERE account_address=%s', (address,))
                    cur.execute('DELETE FROM users WHERE mail_address=%s', (address,))
            db.close(); args.state.unlink(missing_ok=True)

if __name__ == '__main__':
    try: main()
    except KeyboardInterrupt: pass
