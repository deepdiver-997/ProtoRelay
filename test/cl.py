#!/usr/bin/env python3
import argparse, random, string, time, threading, smtplib
from email.mime.text import MIMEText

def make_message(subjects, bodies, sender, recipient):
    subject = random.choice(subjects)
    body = random.choice(bodies)
    msg = MIMEText(body, "plain", "utf-8")
    msg["Subject"] = subject
    msg["From"] = sender
    msg["To"] = recipient
    return msg

def worker(idx, args, senders, recipients, subjects, bodies, stats):
    client = None
    if args.tls:
        client = smtplib.SMTP_SSL(args.host, args.port, timeout=args.timeout)
    else:
        client = smtplib.SMTP(args.host, args.port, timeout=args.timeout)
        if args.starttls:
            client.starttls()
    if args.user and args.password:
        client.login(args.user, args.password)

    for i in range(args.messages // args.concurrency + (1 if idx < args.messages % args.concurrency else 0)):
        sender = random.choice(senders)
        recipient = random.choice(recipients)
        msg = make_message(subjects, bodies, sender, recipient)
        try:
            client.sendmail(sender, [recipient], msg.as_string())
            stats["ok"] += 1
        except Exception as e:
            stats["fail"] += 1
            if args.verbose:
                print(f"[{idx}] send failed: {e}")
        if args.delay > 0:
            time.sleep(args.delay)
    client.quit()

def main():
    parser = argparse.ArgumentParser(description="SMTP stress tester")
    parser.add_argument("--host", default="localhost")
    parser.add_argument("--port", type=int, default=25)
    parser.add_argument("--user", default=None)
    parser.add_argument("--password", default=None)
    parser.add_argument("--tls", action="store_true", help="Use implicit TLS (SMTPS)")
    parser.add_argument("--starttls", action="store_true", help="Upgrade with STARTTLS")
    parser.add_argument("--messages", type=int, default=100, help="Total messages to send")
    parser.add_argument("--concurrency", type=int, default=10, help="Concurrent threads")
    parser.add_argument("--timeout", type=int, default=10)
    parser.add_argument("--delay", type=float, default=0.0, help="Seconds to sleep between sends per thread")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    # Customize these lists as needed
    senders = [f"user{i}@example.com" for i in range(10)]
    recipients = [f"dest{i}@example.net" for i in range(10)]
    subjects = [
        "Load test",
        "Throughput check",
        "SMTP stress run",
        "Benchmark message",
    ]
    bodies = [
        "Short body A",
        "Short body B",
        "This is a slightly longer body to test payload handling.",
    ]

    stats = {"ok": 0, "fail": 0}
    threads = []
    start = time.time()
    for idx in range(args.concurrency):
        t = threading.Thread(target=worker, args=(idx, args, senders, recipients, subjects, bodies, stats))
        t.start()
        threads.append(t)
    for t in threads:
        t.join()
    elapsed = time.time() - start
    print(f"Sent: {stats['ok']} ok, {stats['fail']} failed, elapsed {elapsed:.2f}s, rate {stats['ok']/elapsed:.1f} msg/s")

if __name__ == "__main__":
    main()