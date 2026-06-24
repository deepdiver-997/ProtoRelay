#!/usr/bin/env python3
"""SMTP stress tester — default: connection reuse per worker (original benchmark mode)."""
import argparse, random, time, threading, smtplib, socket, ssl

SENDERS    = [f"user{i}@scut.email" for i in range(10)]
RECIPIENTS = [f"dest{i}@scut.email" for i in range(10)]
MSG_BODY   = "From: {sender}\r\nTo: {rcpt}\r\nSubject: perf test\r\n\r\nshort body\r\n"

def new_client(args):
    """Create a fresh SMTP client with optional TLS/STARTTLS + AUTH."""
    if args.tls or args.port == 465:
        ctx = ssl.create_default_context()
        ctx.check_hostname = False; ctx.verify_mode = ssl.CERT_NONE
        sock = ctx.wrap_socket(socket.socket())
        sock.settimeout(args.timeout)
        sock.connect((args.host, args.port))
        client = smtplib.SMTP()
        client.sock = sock
        client.file = sock.makefile("rb")
    else:
        client = smtplib.SMTP(args.host, args.port, timeout=args.timeout)
        if args.starttls or args.port == 587:
            client.starttls()
    if args.user:
        client.login(args.user, args.password)
    return client

def worker_conn_pool(idx, args, stats, latencies):
    """One connection per worker, reuse for many messages (original benchmark mode)."""
    n = args.messages // args.concurrency + (1 if idx < args.messages % args.concurrency else 0)
    if n == 0: return
    try:
        client = new_client(args)
        for _ in range(n):
            t0 = time.perf_counter()
            sender = random.choice(SENDERS)
            rcpt   = random.choice(RECIPIENTS)
            client.sendmail(sender, [rcpt], MSG_BODY.format(sender=sender, rcpt=rcpt))
            stats["ok"] += 1
            latencies.append((time.perf_counter() - t0) * 1000)
        client.quit()
    except Exception as e:
        stats["fail"] += n

def worker_per_conn(idx, args, stats, latencies):
    """New connection per message (realistic, slower)."""
    n = args.messages // args.concurrency + (1 if idx < args.messages % args.concurrency else 0)
    for _ in range(n):
        t0 = time.perf_counter()
        try:
            client = new_client(args)
            sender = random.choice(SENDERS)
            rcpt   = random.choice(RECIPIENTS)
            client.sendmail(sender, [rcpt], MSG_BODY.format(sender=sender, rcpt=rcpt))
            client.quit()
            stats["ok"] += 1
            latencies.append((time.perf_counter() - t0) * 1000)
        except Exception:
            stats["fail"] += 1

def main():
    p = argparse.ArgumentParser(description="SMTP stress tester")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=25)
    p.add_argument("--user")
    p.add_argument("--password")
    p.add_argument("--tls", action="store_true")
    p.add_argument("--starttls", action="store_true")
    p.add_argument("--messages", type=int, default=1000)
    p.add_argument("--concurrency", type=int, default=50)
    p.add_argument("--timeout", type=int, default=10)
    p.add_argument("--per-conn", action="store_true", help="New connection per message (slower, realistic)")
    p.add_argument("--verbose", action="store_true")
    args = p.parse_args()

    stats = {"ok": 0, "fail": 0}
    latencies = []
    worker_fn = worker_per_conn if getattr(args, 'per_conn', False) else worker_conn_pool
    threads = []
    t0 = time.perf_counter()
    for i in range(args.concurrency):
        t = threading.Thread(target=worker_fn, args=(i, args, stats, latencies))
        t.start(); threads.append(t)
    for t in threads:
        t.join()
    elapsed = time.perf_counter() - t0

    ok = stats["ok"]
    info = f"  total={ok+stats['fail']}  ok={ok}  fail={stats['fail']}  time={elapsed:.1f}s  rate={ok/elapsed:.0f} msg/s"
    if latencies:
        s = sorted(latencies)
        info += f"  p50={s[len(s)//2]:.1f}ms  p99={s[int(len(s)*0.99)]:.1f}ms"
    print(info)

if __name__ == "__main__":
    main()
