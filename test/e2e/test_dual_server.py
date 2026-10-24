#!/usr/bin/env python3
"""
双 SMTP 服务器互通 E2E 测试。

启动两个 smtpsServer 实例：
  Server A (sender relay): --port (MTA), --port+1 (SSL), outbound 静态路由指向 B
  Server B (receiver MX):   --port+2 (MTA, 免认证接受一切)

测试流程:
  1. 用 Python smtplib 向 Server A 投递一封发给 @b.local 的邮件
  2. Server A 通过静态路由直连 Server B 的 10026 端口完成外投
  3. 检查 Server B 的文件系统确认邮件已写入

用法:
  python test/e2e/test_dual_server.py [--server ./build/smtpsServer] [--port 20300]
      [--backend asio|native] [--protocol-only] [--auth-sqlite]
"""

import argparse, json, os, shutil, signal, socket, smtplib, sqlite3, ssl, subprocess, sys, tempfile, time


def load_json(path):
    with open(path) as f: return json.load(f)

def make_temp_config(base_cfg, proj_root, overrides):
    """复制一份配置到临时目录，覆盖指定字段"""
    import copy
    cfg = copy.deepcopy(base_cfg)
    cfg.update(overrides)
    cfg['perf_mode'] = False
    cfg['metrics_enabled'] = False
    cfg['log_to_console'] = False
    cfg['log_to_file'] = True
    # 转绝对路径
    for k in ('certFile', 'keyFile', 'dhFile'):
        if cfg.get(k) and not os.path.isabs(cfg[k]):
            cfg[k] = os.path.join(proj_root, cfg[k])
    if cfg.get('db_config_file') and not os.path.isabs(cfg['db_config_file']):
        cfg['db_config_file'] = os.path.join(proj_root, cfg['db_config_file'])
    return cfg

def start_server(bin_path, cfg, mail_dir, proj_root):
    """启动服务器，返回 subprocess.Popen"""
    os.makedirs(mail_dir, exist_ok=True)
    cfg['storage'] = {'provider': 'local', 'local': {
        'mail_path': mail_dir, 'attachment_path': os.path.join(mail_dir, 'att')}}
    cfg['log_file'] = os.path.join(mail_dir, 'server.log')

    tmpdir = os.path.dirname(mail_dir)
    config_file = os.path.join(mail_dir, 'config.json')
    with open(config_file, 'w') as f:
        json.dump(cfg, f)

    proc = subprocess.Popen(
        [bin_path, '-c', config_file],
        cwd=proj_root,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        preexec_fn=os.setsid)
    time.sleep(3)
    return proc, tmpdir

def stop_server(proc):
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
        proc.wait(timeout=5)
    except: pass

def verify_mail(mail_dir):
    files = [f for f in os.listdir(mail_dir)
             if f not in ('att', 'server.log', 'config.json') and os.path.isfile(os.path.join(mail_dir, f))]
    assert files, f"no mail files in {mail_dir}"
    newest = max(files, key=lambda f: os.path.getmtime(os.path.join(mail_dir, f)))
    size = os.path.getsize(os.path.join(mail_dir, newest))
    assert size > 50, f"mail file too small ({size} bytes)"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--server', default='./build/smtpsServer')
    parser.add_argument('--config', default='config/smtpsConfig.json')
    parser.add_argument('--port', type=int, default=10025)
    parser.add_argument('--backend', choices=('asio', 'native'), default='asio')
    parser.add_argument('--auth-sqlite', action='store_true', help='verify SMTP AUTH against an isolated SQLite account')
    parser.add_argument('--protocol-only', action='store_true', help='check SMTP transport and AUTH without outbound delivery')
    args = parser.parse_args()

    script_dir = os.path.dirname(os.path.abspath(__file__))
    proj_root = os.path.normpath(os.path.join(script_dir, '..', '..'))
    server_bin = os.path.join(proj_root, args.server)
    base_cfg = load_json(os.path.join(proj_root, args.config))
    port_a, port_tls, port_b = args.port, args.port + 1, args.port + 2

    if not os.path.exists(server_bin):
        print(f"ERROR: {server_bin} not found. Build first."); sys.exit(1)

    # ---- Server A: sender relay ----
    # outbound 静态路由: b.local → Server B
    temp_root = os.environ.get('AGENT_TMP_ROOT')
    if temp_root: os.makedirs(temp_root, exist_ok=True)
    mail_a = tempfile.mkdtemp(prefix='smtp_a_', dir=temp_root)
    cfg_a = make_temp_config(base_cfg, proj_root, {
        'listeners': [
            {'type': 'tcp', 'port': port_a, 'auth_policy': 'off', 'advertise_starttls': True},
            {'type': 'ssl', 'port': port_tls, 'auth_policy': 'off'},
        ],
        'system_domain': 'a.local',
        'external_delivery_enabled': True,
        'trusted_relay_networks': ['127.0.0.1'],
        'outbound': {
            'helo_domain': 'a.local',
            'ports': [25],
            'static_routes': {'b.local': {'host': '127.0.0.1', 'port': port_b}},
            'dkim': {'enabled': False},
        },
        'use_database': False,
        'io_backend': args.backend,
        'address': '127.0.0.1',
    })

    # ---- Server B: receiver MX (port 10026 MTA, no auth) ----
    mail_b = tempfile.mkdtemp(prefix='smtp_b_', dir=temp_root)
    cfg_b = make_temp_config(base_cfg, proj_root, {
        'listeners': [
            {'type': 'tcp', 'port': port_b, 'auth_policy': 'off'},
        ],
        'system_domain': 'b.local',
        'use_database': False,
        'io_backend': args.backend,
        'address': '127.0.0.1',
    })

    proc_a, tmp_a = None, None
    proc_b, tmp_b = None, None
    proc_auth = None
    auth_dir = None

    try:
        print(f"Starting Server A (sender) on :{port_a}...")
        proc_a, tmp_a = start_server(server_bin, cfg_a, mail_a, proj_root)

        print(f"Starting Server B (receiver) on :{port_b}...")
        proc_b, tmp_b = start_server(server_bin, cfg_b, mail_b, proj_root)

        # ---- 验证连通性 ----
        s = socket.create_connection(('127.0.0.1', port_a), timeout=3)
        banner = s.recv(512).decode()
        assert banner.startswith('220'), f"Server A banner: {banner[:60]}"
        s.close()

        s = socket.create_connection(('127.0.0.1', port_b), timeout=3)
        banner = s.recv(512).decode()
        assert banner.startswith('220'), f"Server B banner: {banner[:60]}"
        s.close()

        # ---- 通过 Server A 投递到 b.local ----
        tls_context = ssl._create_unverified_context()
        with smtplib.SMTP_SSL('127.0.0.1', port_tls, timeout=10,
                              context=tls_context) as tls_smtp:
            code, _ = tls_smtp.ehlo('test.a.local')
            assert code == 250, f"SMTP TLS EHLO: {code}"
        with smtplib.SMTP('127.0.0.1', port_a, timeout=10) as starttls_smtp:
            code, _ = starttls_smtp.ehlo('test.a.local')
            assert code == 250, f"SMTP EHLO: {code}"
            code, _ = starttls_smtp.starttls(context=tls_context)
            assert code == 220, f"SMTP STARTTLS: {code}"
            code, _ = starttls_smtp.ehlo('test.a.local')
            assert code == 250, f"SMTP EHLO after STARTTLS: {code}"
        if not args.protocol_only:
            smtp = smtplib.SMTP('127.0.0.1', port_a, timeout=10)
            smtp.ehlo('test.a.local')
            smtp.sendmail('sender@external.local', 'user@b.local',
                'From: sender@external.local\r\nTo: user@b.local\r\nSubject: Dual Server Test\r\n\r\nDelivered via static route.')
            smtp.quit()
            print("Mail submitted to Server A. Waiting for outbound delivery...")
            time.sleep(3)

            # ---- 验证 Server B 收到邮件 ----
            try:
                verify_mail(mail_b)
            except AssertionError:
                for label, path in (('sender', mail_a), ('receiver', mail_b)):
                    log_path = os.path.join(path, 'server.log')
                    if os.path.exists(log_path):
                        with open(log_path, errors='replace') as f:
                            print(f'{label} log tail:\n' + ''.join(f.readlines()[-20:]))
                raise
            print("Server B received mail ✓")

        if args.auth_sqlite:
            auth_dir = tempfile.mkdtemp(prefix='smtp_auth_', dir=temp_root)
            db_path = os.path.join(auth_dir, 'mail.sqlite')
            user, password = 'native-deploy@test.local', 'native-deploy-e2e'
            with sqlite3.connect(db_path) as db:
                db.execute('CREATE TABLE users (id INTEGER PRIMARY KEY, mail_address TEXT UNIQUE, password TEXT, status INTEGER, last_login_time TEXT)')
                db.execute('INSERT INTO users (mail_address, password, status) VALUES (?, ?, 1)',
                           (user, password))
            db_cfg = os.path.join(auth_dir, 'db.json')
            with open(db_cfg, 'w') as f:
                json.dump({'achieve': 'sqlite', 'database': db_path,
                           'initial_pool_size': 2, 'max_pool_size': 4}, f)
            cfg_auth = make_temp_config(base_cfg, proj_root, {
                'listeners': [{'type': 'tcp', 'port': port_b + 1, 'auth_policy': 'on'}],
                'address': '127.0.0.1', 'io_backend': args.backend,
                'use_database': True, 'db_config_file': db_cfg,
            })
            proc_auth, _ = start_server(server_bin, cfg_auth,
                                        os.path.join(auth_dir, 'mail'), proj_root)
            with smtplib.SMTP('127.0.0.1', port_b + 1, timeout=10) as auth_smtp:
                auth_smtp.ehlo('test.a.local')
                auth_smtp.login(user, password)
            print("SMTP AUTH with SQLite account ✓")

        print("\n=== SMTP protocol E2E test PASSED ===" if args.protocol_only
              else "\n=== Dual-server delivery E2E test PASSED ===")

    finally:
        for p in [proc_auth, proc_a, proc_b]:
            if p: stop_server(p)
        for path in (mail_a, mail_b, auth_dir):
            if path: shutil.rmtree(path, ignore_errors=True)


if __name__ == '__main__':
    main()
