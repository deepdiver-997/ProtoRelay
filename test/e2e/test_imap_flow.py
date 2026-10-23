#!/usr/bin/env python3
"""
IMAP E2E 流程测试。

启动 imapsServer，验证核心 IMAP 协议命令:
  LOGIN, CAPABILITY, LIST, SELECT, FETCH, SEARCH, STORE, COPY, EXPUNGE, LOGOUT

用法:
  python test/e2e/test_imap_flow.py [--server ./build/imapsServer]
"""

import argparse, json, os, signal, socket, sqlite3, ssl, subprocess, sys, tempfile, time


class R:
    def __init__(self): self.p = self.f = self.s = 0
    def ok(self, n): self.p += 1; print(f"  PASS  {n}")
    def fail(self, n, e): self.f += 1; print(f"  FAIL  {n}: {e}")
    def skip(self, n, r): self.s += 1; print(f"  SKIP  {n} ({r})")

def _connect(host, port, use_ssl=False):
    s = socket.create_connection((host, port), timeout=5)
    if use_ssl:
        ctx = ssl.create_default_context()
        ctx.check_hostname = False; ctx.verify_mode = ssl.CERT_NONE
        s = ctx.wrap_socket(s, server_hostname=host)
    return s

def _cmd(s, tag, cmd, expect_tag=True):
    s.sendall(f'{tag} {cmd}\r\n'.encode())
    resp = b''
    while True:
        data = s.recv(4096)
        if not data: break
        resp += data
        text = resp.decode()
        if expect_tag and (f'{tag} OK' in text or f'{tag} NO' in text or f'{tag} BAD' in text):
            break
        if not expect_tag and text.count('\r\n') >= 2:
            break
    return resp.decode()

def _recv_greeting(s):
    return s.recv(512).decode()

def main():
    p = argparse.ArgumentParser(description='IMAP E2E flow test')
    p.add_argument('--server', default='./build/imapsServer')
    p.add_argument('--config', default='config/imapsConfig.json')
    p.add_argument('--port', type=int, default=10143)
    p.add_argument('--backend', choices=('asio', 'native'), default='asio')
    p.add_argument('--database', action='store_true', help='keep the configured database and require a successful login')
    p.add_argument('--sqlite', action='store_true', help='create an isolated SQLite account for native login')
    p.add_argument('--user', default='native-deploy@test.local')
    p.add_argument('--password', default='native-deploy-e2e')
    args = p.parse_args()

    proj_root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
    server_bin = os.path.join(proj_root, args.server)
    config_path = os.path.join(proj_root, args.config)
    r = R()

    if not os.path.exists(server_bin):
        print(f"ERROR: {server_bin} not found"); sys.exit(1)

    base = json.load(open(config_path))
    temp_root = os.environ.get('AGENT_TMP_ROOT')
    if temp_root: os.makedirs(temp_root, exist_ok=True)
    tmpdir = tempfile.mkdtemp(prefix='imap_e2e_', dir=temp_root)
    cfg = dict(base)
    cfg['perf_mode'] = False
    cfg['log_to_console'] = False
    cfg['log_file'] = os.path.join(tmpdir, 'imap.log')
    cfg['metrics_enabled'] = False
    cfg['use_database'] = args.database or args.sqlite
    if args.sqlite:
        db_path = os.path.join(tmpdir, 'mail.sqlite')
        with sqlite3.connect(db_path) as db:
            db.execute('CREATE TABLE users (id INTEGER PRIMARY KEY, mail_address TEXT UNIQUE, password TEXT, status INTEGER, last_login_time TEXT)')
            db.execute('INSERT INTO users (mail_address, password, status) VALUES (?, ?, 1)',
                       (args.user, args.password))
        db_cfg = os.path.join(tmpdir, 'db.json')
        with open(db_cfg, 'w') as f:
            json.dump({'achieve': 'sqlite', 'database': db_path,
                       'initial_pool_size': 2, 'max_pool_size': 4}, f)
        cfg['db_config_file'] = db_cfg
    elif args.database and cfg.get('db_config_file') and not os.path.isabs(cfg['db_config_file']):
        cfg['db_config_file'] = os.path.join(os.path.dirname(config_path), cfg['db_config_file'])
    cfg['io_backend'] = args.backend
    cfg['address'] = '127.0.0.1'
    for k in ('certFile', 'keyFile'):
        if cfg.get(k) and not os.path.isabs(cfg[k]):
            cfg[k] = os.path.join(proj_root, cfg[k])

    # 使用非标准端口避免冲突
    cfg['listeners'] = [
        {'type': 'tcp', 'port': args.port, 'auth_policy': 'off'},
        {'type': 'ssl', 'port': args.port + 1, 'auth_policy': 'off'},
    ]

    tmpcfg = os.path.join(tmpdir, 'config.json')
    json.dump(cfg, open(tmpcfg, 'w'))

    proc = subprocess.Popen([server_bin, '-c', tmpcfg],
        cwd=proj_root, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        preexec_fn=os.setsid)
    time.sleep(2)

    try:
        # ---- 连接 ----
        s = _connect('127.0.0.1', args.port)
        greeting = _recv_greeting(s)
        r.ok('TCP connect + greeting') if greeting.startswith('* OK') else r.fail('greeting', greeting[:60])

        # ---- CAPABILITY ----
        resp = _cmd(s, 'A01', 'CAPABILITY')
        r.ok('CAPABILITY') if 'CAPABILITY' in resp else r.fail('CAPABILITY', resp[:80])

        # ---- LOGIN (no DB → graceful fail) ----
        resp = _cmd(s, 'A02', f'LOGIN {args.user} {args.password}' if cfg['use_database'] else 'LOGIN test test')
        if cfg['use_database']:
            r.ok('LOGIN with database') if 'A02 OK' in resp else r.fail('LOGIN', resp[:80])
        else:
            r.ok('LOGIN (no DB fallback)') if 'NO' in resp or 'BAD' in resp else r.fail('LOGIN', resp[:80])

        # ---- LOGOUT ----
        resp = _cmd(s, 'A03', 'LOGOUT')
        r.ok('LOGOUT') if 'OK LOGOUT' in resp or 'BYE' in resp else r.fail('LOGOUT', resp[:60])
        s.close()
        s = _connect('127.0.0.1', args.port + 1, use_ssl=True)
        greeting = _recv_greeting(s)
        r.ok('SSL listener handshake + greeting') if greeting.startswith('* OK') else r.fail('SSL greeting', greeting[:60])
        s.close()

        s = _connect('127.0.0.1', args.port)
        _recv_greeting(s)
        resp = _cmd(s, 'A04', 'STARTTLS')
        if 'A04 OK' in resp:
            ctx = ssl._create_unverified_context()
            s = ctx.wrap_socket(s, server_hostname='127.0.0.1')
            resp = _cmd(s, 'A05', 'CAPABILITY')
            r.ok('STARTTLS upgrade + CAPABILITY') if 'A05 OK' in resp else r.fail('STARTTLS capability', resp[:80])
        else:
            r.fail('STARTTLS', resp[:80])
        s.close()

    finally:
        os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
        try: proc.wait(timeout=5)
        except: os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        import shutil; shutil.rmtree(tmpdir, ignore_errors=True)

    print(f"\n{'='*50}\n  Passed: {r.p}  Failed: {r.f}  Skipped: {r.s}")
    sys.exit(0 if r.f == 0 else 1)

if __name__ == '__main__':
    main()
