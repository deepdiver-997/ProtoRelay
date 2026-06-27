#!/usr/bin/env python3
"""
SMTP stress & benchmark tester — multi-mode, multi-strategy, ramp concurrency.

Modes (auto-configure port/TLS/auth):
  mta-relay     Port 25, no TLS, no auth — simulates MTA-to-MTA relay
  submission    Port 587, STARTTLS + AUTH LOGIN — simulates MUA submission
  smtps         Port 465, implicit TLS + AUTH LOGIN — SMTPS

Strategies:
  conn-pool     One connection per thread, reuse for many messages (highest throughput)
  per-conn      New connection per message (realistic, slower)
  pipeline      Raw socket, batch all commands into one TCP write (absolute max)

Usage:
  # Single run: MTA relay with connection pool
  python cl.py --mode mta-relay --messages 20000 --concurrency 200

  # Single run: submission pipeline
  python cl.py --mode submission --user me@scut.email --password x --strategy pipeline

  # Ramp: find server limit by increasing concurrency
  python cl.py --mode mta-relay --ramp --ramp-start 50 --ramp-end 500 --ramp-step 50

  # Scan: run all mode×strategy combos
  python cl.py --scan --messages 5000

  # Legacy flat args still work
  python cl.py --port 25 --messages 10000 --concurrency 100
"""

import argparse, base64, random, time, threading, smtplib, socket, ssl, sys, statistics

# ── test data ──────────────────────────────────────────────────────────────
SENDERS    = [f"user{i}@scut.email" for i in range(10)]
RECIPIENTS = [f"dest{i}@scut.email" for i in range(10)]
MSG_BODY   = "From: {sender}\r\nTo: {rcpt}\r\nSubject: perf test\r\n\r\nshort body\r\n"

# ── mode presets ───────────────────────────────────────────────────────────
MODE_PRESETS = {
    "mta-relay":  {"port": 25,  "tls": False, "starttls": False, "auth": False, "label": "MTA relay (port 25, no auth)"},
    "submission": {"port": 587, "tls": False, "starttls": True,  "auth": True,  "label": "Submission (port 587, STARTTLS+AUTH)"},
    "smtps":      {"port": 465, "tls": True,  "starttls": False, "auth": True,  "label": "SMTPS (port 465, TLS+AUTH)"},
}


# ── client creation ────────────────────────────────────────────────────────
def make_client(args):
    """Create SMTP client from args (handles TLS/STARTTLS/AUTH)."""
    timeout = getattr(args, "timeout", 10)
    if args.tls or args.port == 465:
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        sock = ctx.wrap_socket(socket.socket())
        sock.settimeout(timeout)
        sock.connect((args.host, args.port))
        client = smtplib.SMTP()
        client.sock = sock
        client.file = sock.makefile("rb")
    else:
        client = smtplib.SMTP(args.host, args.port, timeout=timeout)
        if args.starttls or args.port == 587:
            client.starttls()
    if args.user:
        client.login(args.user, args.password)
    return client


# ── strategy: conn-pool ────────────────────────────────────────────────────
def worker_conn_pool(idx, args, stats, latencies):
    """One connection per thread, reused for many messages."""
    n = args.messages // args.concurrency + (1 if idx < args.messages % args.concurrency else 0)
    if n == 0:
        return
    try:
        client = make_client(args)
        for _ in range(n):
            sender = random.choice(SENDERS)
            rcpt = random.choice(RECIPIENTS)
            t0 = time.perf_counter()
            client.sendmail(sender, [rcpt], MSG_BODY.format(sender=sender, rcpt=rcpt))
            stats["ok"] += 1
            latencies.append((time.perf_counter() - t0) * 1000)
        client.quit()
    except Exception as e:
        stats["fail"] += n
        if args.verbose:
            print(f"  [w{idx}] conn-pool error: {e}")


# ── strategy: per-conn ─────────────────────────────────────────────────────
def worker_per_conn(idx, args, stats, latencies):
    """New connection per message, same batched-write path as pipeline strategy.

    Unlike smtplib (which serializes EHLO→MAIL→RCPT→DATA per message),
    this uses raw sockets to batch all SMTP commands in one write.
    The only overhead vs conn-pool is TCP+TLS+AUTH per message.
    """
    # Delegate to the same pipeline implementation — the difference between
    # per-conn and pipeline is now just a naming convention. Both create a
    # fresh connection per message and batch commands.
    worker_pipeline(idx, args, stats, latencies)


def _shutdown_tls(sock):
    """Shut down TLS connection gracefully, sending close_notify before TCP close."""
    if sock is None:
        return
    try:
        sock.shutdown(socket.SHUT_RDWR)
    except Exception:
        pass
    try:
        sock.close()
    except Exception:
        pass


# ── strategy: pipeline ─────────────────────────────────────────────────────
def _read_multiline_response(f, timeout=10):
    """Read an SMTP multiline response (lines ending with 'XXX ' instead of 'XXX-')."""
    while True:
        line = f.readline()
        if not line:
            raise ConnectionError("connection closed during multiline response")
        line_str = line.decode("utf-8", errors="replace") if isinstance(line, bytes) else line
        if len(line_str) >= 4 and line_str[3] == " ":
            return line_str


def _read_single_response(f, timeout=10):
    """Read a single SMTP response line."""
    line = f.readline()
    if not line:
        raise ConnectionError("connection closed")
    return line.decode("utf-8", errors="replace") if isinstance(line, bytes) else line


def _pipeline_plain(args, stats, latencies):
    """Pipeline on plain TCP port 25 — no TLS, no auth."""
    ok, fail = 0, 0
    lats = []
    for _ in range(args.messages // args.concurrency):
        t0 = time.perf_counter()
        sock = None
        try:
            sock = socket.socket()
            sock.settimeout(args.timeout)
            sock.connect((args.host, args.port))
            f = sock.makefile("rb")

            banner = _read_single_response(f)
            if not banner.startswith("220"):
                raise Exception(f"bad banner: {banner.strip()}")

            sender = random.choice(SENDERS)
            rcpt = random.choice(RECIPIENTS)
            body = MSG_BODY.format(sender=sender, rcpt=rcpt)

            cmds = (
                f"EHLO test\r\n"
                f"MAIL FROM:<{sender}>\r\n"
                f"RCPT TO:<{rcpt}>\r\n"
                f"DATA\r\n"
                f"{body}\r\n.\r\n"
                f"QUIT\r\n"
            )
            sock.sendall(cmds.encode())

            _read_multiline_response(f)       # EHLO
            r = _read_single_response(f)       # MAIL FROM
            if not r.startswith("250"): raise Exception(f"MAIL FROM: {r.strip()}")
            r = _read_single_response(f)       # RCPT TO
            if not r.startswith("250"): raise Exception(f"RCPT TO: {r.strip()}")
            r = _read_single_response(f)       # DATA (354)
            if not r.startswith("354"): raise Exception(f"DATA: {r.strip()}")
            r = _read_single_response(f)       # accept (250)
            if not r.startswith("250"): raise Exception(f"accept: {r.strip()}")
            ok += 1
            lats.append((time.perf_counter() - t0) * 1000)
        except Exception as e:
            fail += 1
            if args.verbose:
                print(f"  pipeline-plain error: {e}")
        finally:
            _shutdown_tls(sock)

    stats["ok"] += ok
    stats["fail"] += fail
    latencies.extend(lats)


def _pipeline_starttls(args, stats, latencies):
    """Pipeline with STARTTLS (port 587): STARTTLS handshake first, then pipeline."""
    ok, fail = 0, 0
    lats = []
    for _ in range(args.messages // args.concurrency):
        t0 = time.perf_counter()
        sock = None
        try:
            sock = socket.socket()
            sock.settimeout(args.timeout)
            sock.connect((args.host, args.port))
            f = sock.makefile("rb")

            banner = _read_single_response(f)
            if not banner.startswith("220"):
                raise Exception(f"bad banner: {banner.strip()}")

            # EHLO + STARTTLS handshake (must be sequential, can't pipeline TLS upgrade)
            sock.sendall(b"EHLO test\r\n")
            _read_multiline_response(f)

            sock.sendall(b"STARTTLS\r\n")
            ready = _read_single_response(f)
            if not ready.startswith("220"):
                raise Exception(f"STARTTLS rejected: {ready.strip()}")

            # TLS upgrade — RFC 3207: discard prior state, must re-EHLO
            ctx = ssl.create_default_context()
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            sock = ctx.wrap_socket(sock)
            f = sock.makefile("rb")

            sock.sendall(b"EHLO test\r\n")
            _read_multiline_response(f)
            sock.sendall(b"AUTH LOGIN\r\n")
            r = _read_single_response(f)
            if not r.startswith("334"):
                raise Exception(f"AUTH prompt: {r.strip()}")
            sock.sendall(base64.b64encode(args.user.encode()) + b"\r\n")
            r = _read_single_response(f)
            sock.sendall(base64.b64encode(args.password.encode()) + b"\r\n")
            r = _read_single_response(f)
            if not r.startswith("235"):
                raise Exception(f"AUTH failed: {r.strip()}")

            # Now pipeline the rest after auth
            sender = random.choice(SENDERS)
            rcpt = random.choice(RECIPIENTS)
            body = MSG_BODY.format(sender=sender, rcpt=rcpt)

            cmds = (
                f"MAIL FROM:<{sender}>\r\n"
                f"RCPT TO:<{rcpt}>\r\n"
                f"DATA\r\n"
                f"{body}\r\n.\r\n"
                f"QUIT\r\n"
            )
            sock.sendall(cmds.encode())

            r = _read_single_response(f)
            if not r.startswith("250"): raise Exception(f"MAIL FROM: {r.strip()}")
            r = _read_single_response(f)
            if not r.startswith("250"): raise Exception(f"RCPT TO: {r.strip()}")
            r = _read_single_response(f)
            if not r.startswith("354"): raise Exception(f"DATA: {r.strip()}")
            r = _read_single_response(f)
            if not r.startswith("250"): raise Exception(f"accept: {r.strip()}")
            # 250 OK 已收到，邮件投递成功；不等 QUIT 响应，直接 TLS 优雅关闭
            ok += 1
            lats.append((time.perf_counter() - t0) * 1000)
        except Exception as e:
            fail += 1
            if args.verbose:
                print(f"  pipeline-starttls error: {e}")
        finally:
            _shutdown_tls(sock)

    stats["ok"] += ok
    stats["fail"] += fail
    latencies.extend(lats)


def _pipeline_tls(args, stats, latencies):
    """Pipeline with implicit TLS (port 465): TLS first, AUTH, then pipeline."""
    ok, fail = 0, 0
    lats = []
    for _ in range(args.messages // args.concurrency):
        t0 = time.perf_counter()
        sock = None
        try:
            sock = socket.socket()
            sock.settimeout(args.timeout)
            sock.connect((args.host, args.port))

            ctx = ssl.create_default_context()
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            sock = ctx.wrap_socket(sock)
            f = sock.makefile("rb")

            banner = _read_single_response(f)
            if not banner.startswith("220"):
                raise Exception(f"bad banner: {banner.strip()}")

            # EHLO required after TLS connect
            sock.sendall(b"EHLO test\r\n")
            _read_multiline_response(f)

            # AUTH LOGIN (sequential)
            sock.sendall(b"AUTH LOGIN\r\n")
            r = _read_single_response(f)
            if not r.startswith("334"):
                raise Exception(f"AUTH prompt: {r.strip()}")
            sock.sendall(base64.b64encode(args.user.encode()) + b"\r\n")
            r = _read_single_response(f)
            sock.sendall(base64.b64encode(args.password.encode()) + b"\r\n")
            r = _read_single_response(f)
            if not r.startswith("235"):
                raise Exception(f"AUTH failed: {r.strip()}")

            # Pipeline after auth
            sender = random.choice(SENDERS)
            rcpt = random.choice(RECIPIENTS)
            body = MSG_BODY.format(sender=sender, rcpt=rcpt)

            cmds = (
                f"MAIL FROM:<{sender}>\r\n"
                f"RCPT TO:<{rcpt}>\r\n"
                f"DATA\r\n"
                f"{body}\r\n.\r\n"
                f"QUIT\r\n"
            )
            sock.sendall(cmds.encode())

            r = _read_single_response(f)
            if not r.startswith("250"): raise Exception(f"MAIL FROM: {r.strip()}")
            r = _read_single_response(f)
            if not r.startswith("250"): raise Exception(f"RCPT TO: {r.strip()}")
            r = _read_single_response(f)
            if not r.startswith("354"): raise Exception(f"DATA: {r.strip()}")
            r = _read_single_response(f)
            if not r.startswith("250"): raise Exception(f"accept: {r.strip()}")
            ok += 1
            lats.append((time.perf_counter() - t0) * 1000)
        except Exception as e:
            fail += 1
            if args.verbose:
                print(f"  pipeline-smtps error: {e}")
        finally:
            _shutdown_tls(sock)

    stats["ok"] += ok
    stats["fail"] += fail
    latencies.extend(lats)


def worker_pipeline(idx, args, stats, latencies):
    """
    Pipeline strategy: dispatch to correct pipeline variant based on mode.
    Each worker processes messages // concurrency messages.
    """
    if args.tls or args.port == 465:
        _pipeline_tls(args, stats, latencies)
    elif args.starttls or args.port == 587:
        _pipeline_starttls(args, stats, latencies)
    else:
        _pipeline_plain(args, stats, latencies)


# ── run helpers ────────────────────────────────────────────────────────────
def fmt_latency(lats):
    """Format latency percentiles."""
    if not lats:
        return ""
    s = sorted(lats)
    n = len(s)
    def p(pct):
        return s[min(n - 1, int(n * pct))]
    return f"  p50={p(0.50):.1f}ms  p99={p(0.99):.1f}ms  p999={p(0.999):.1f}ms"


def run_benchmark(args, label="") -> dict:
    """Run a single benchmark with the given args, return stats dict."""
    stats = {"ok": 0, "fail": 0}
    latencies = []

    strategy = getattr(args, "strategy", "conn-pool")
    worker_map = {
        "conn-pool": worker_conn_pool,
        "per-conn":  worker_per_conn,
        "pipeline":  worker_pipeline,
    }
    worker_fn = worker_map.get(strategy, worker_conn_pool)

    threads = []
    t0 = time.perf_counter()
    for i in range(args.concurrency):
        t = threading.Thread(target=worker_fn, args=(i, args, stats, latencies))
        t.start()
        threads.append(t)
    for t in threads:
        t.join()
    elapsed = time.perf_counter() - t0

    ok, fail = stats["ok"], stats["fail"]
    total = ok + fail
    result = {
        "total": total, "ok": ok, "fail": fail,
        "elapsed": elapsed, "rate": ok / elapsed if elapsed > 0 else 0,
        "error_rate": fail / total * 100 if total > 0 else 0,
    }
    prefix = f"[{label}] " if label else ""
    info = (f"{prefix}total={total}  ok={ok}  fail={fail}  "
            f"time={elapsed:.1f}s  rate={ok/elapsed:.0f} msg/s")
    if latencies:
        info += fmt_latency(latencies)
    print(info)
    return result


def run_ramp(args):
    """Ramp concurrency from ramp_start to ramp_end by ramp_step."""
    print(f"\n{'='*70}")
    print(f"RAMP MODE: {args.ramp_start} → {args.ramp_end} threads (step {args.ramp_step})")
    print(f"Mode={args.mode}  Strategy={args.strategy}  Messages={args.messages}")
    print(f"{'='*70}")

    best_concurrency = 0
    best_rate = 0
    results = []

    for conc in range(args.ramp_start, args.ramp_end + 1, args.ramp_step):
        run_args = argparse.Namespace(**vars(args))
        run_args.concurrency = conc
        label = f"conc={conc}"
        r = run_benchmark(run_args, label)
        r["concurrency"] = conc
        results.append(r)

        if r["rate"] > best_rate:
            best_rate = r["rate"]
            best_concurrency = conc

    # Summary
    print(f"\n{'='*70}")
    print(f"{'Conc':<8} {'Rate (msg/s)':<15} {'Latency avg':<15} {'Err%':<8}")
    print(f"{'-'*46}")
    for r in results:
        print(f"{r['concurrency']:<8} {r['rate']:<15.0f} {r['elapsed']/r['total']*1000 if r['total'] else 0:<15.1f} {r['error_rate']:<8.1f}")
    print(f"\nPeak throughput: {best_rate:.0f} msg/s at concurrency={best_concurrency}")


def run_scan(args):
    """Run all mode × strategy combinations."""
    modes = ["mta-relay", "submission", "smtps"]
    strategies = ["conn-pool", "per-conn", "pipeline"]

    print(f"\n{'='*70}")
    print(f"SCAN MODE: {len(modes)} modes × {len(strategies)} strategies")
    print(f"Messages={args.messages}  Concurrency={args.concurrency}")
    print(f"{'='*70}")

    results = []
    for mode in modes:
        for strat in strategies:
            preset = MODE_PRESETS[mode]
            # Skip incompatible combos
            if strat == "pipeline" and preset["auth"] and not args.user:
                print(f"\n  SKIP {mode}/{strat} — pipeline with auth needs --user/--password")
                continue

            run_args = argparse.Namespace(**vars(args))
            run_args.mode = mode
            run_args.strategy = strat
            run_args.port = preset["port"]
            run_args.tls = preset["tls"]
            run_args.starttls = preset["starttls"]
            if preset["auth"]:
                run_args.user = args.user
                run_args.password = args.password

            label = f"{mode}/{strat}"
            r = run_benchmark(run_args, label)
            r["mode"] = mode
            r["strategy"] = strat
            results.append(r)

    # Summary table
    print(f"\n{'='*80}")
    print(f"{'Mode':<15} {'Strategy':<12} {'Messages':<10} {'Rate (msg/s)':<15} {'Err%':<8}")
    print(f"{'-'*60}")
    for r in results:
        print(f"{r.get('mode',''):<15} {r.get('strategy',''):<12} {r['total']:<10} {r['rate']:<15.0f} {r['error_rate']:<8.1f}")


# ── main ───────────────────────────────────────────────────────────────────
def main():
    p = argparse.ArgumentParser(
        description="SMTP stress & benchmark tester — multi-mode, multi-strategy, ramp concurrency",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""Examples:
  python cl.py --mode mta-relay --messages 20000 --concurrency 200
  python cl.py --mode submission --user me@scut.email --password x --strategy pipeline
  python cl.py --mode mta-relay --ramp --ramp-start 50 --ramp-end 500 --ramp-step 50
  python cl.py --scan --messages 5000 --user me@scut.email --password x
        """,
    )
    # Mode
    p.add_argument("--mode", choices=["mta-relay", "submission", "smtps"],
                   help="Pre-configured mode (auto-sets port/TLS/auth)")
    # Server
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=25)
    # Auth (for submission / smtps modes)
    p.add_argument("--user")
    p.add_argument("--password")
    # TLS flags (overridden by --mode)
    p.add_argument("--tls", action="store_true", help="Implicit TLS (port 465)")
    p.add_argument("--starttls", action="store_true", help="STARTTLS upgrade (port 587)")
    # Workload
    p.add_argument("--messages", type=int, default=1000, help="Total messages to send")
    p.add_argument("--concurrency", type=int, default=50, help="Number of concurrent threads")
    p.add_argument("--timeout", type=int, default=10, help="Socket timeout in seconds")
    # Strategy
    p.add_argument("--strategy", choices=["conn-pool", "per-conn", "pipeline"],
                   default="conn-pool",
                   help="Delivery strategy (default: conn-pool)")
    # Ramp mode
    p.add_argument("--ramp", action="store_true",
                   help="Ramp concurrency to find server limit")
    p.add_argument("--ramp-start", type=int, default=50)
    p.add_argument("--ramp-end", type=int, default=500)
    p.add_argument("--ramp-step", type=int, default=50)
    # Scan mode
    p.add_argument("--scan", action="store_true",
                   help="Run all mode×strategy combinations")
    # Misc
    p.add_argument("--per-conn", action="store_true",
                   help="[legacy] Shortcut for --strategy per-conn")
    p.add_argument("--verbose", action="store_true")

    args = p.parse_args()

    # Apply mode preset
    if args.mode:
        preset = MODE_PRESETS[args.mode]
        args.port = preset["port"]
        args.tls = preset["tls"]
        args.starttls = preset["starttls"]
        if preset["auth"] and not args.user:
            p.error(f"--mode {args.mode} requires --user and --password")
    else:
        args.mode = "custom"

    # Legacy --per-conn compatibility
    if args.per_conn:
        args.strategy = "per-conn"

    # Pipeline strategy doesn't use concurrency the same way — each worker
    # processes messages/concurrency iterations. Ensure divisible.
    if args.strategy == "pipeline":
        if args.messages % args.concurrency != 0:
            adjusted = args.messages - (args.messages % args.concurrency)
            print(f"Note: adjusting --messages {args.messages} → {adjusted} (must be divisible by concurrency)")
            args.messages = adjusted

    # Dispatch
    if args.scan:
        run_scan(args)
    elif args.ramp:
        run_ramp(args)
    else:
        label = f"{args.mode}/{args.strategy}"
        run_benchmark(args, label)


if __name__ == "__main__":
    main()
