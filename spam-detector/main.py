#!/usr/bin/env python3
"""
ProtoRelay Spam Detector
独立进程，从 mails 表租赁未检测邮件 → 正文检测 → 打标签。

状态机: 0=未检测 → 3=检测中 → 1=正常 / 2=垃圾
租赁模式: SELECT FOR UPDATE SKIP LOCKED，天然支持多实例分布式部署。
"""

import json, os, re, time, sys
import mysql.connector
from datetime import datetime

BASE_DIR = os.path.dirname(os.path.abspath(__file__))

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------
with open(os.path.join(BASE_DIR, "config.json")) as f:
    CONFIG = json.load(f)

DB = CONFIG["db"]
POLL_INTERVAL = CONFIG["poll_interval_sec"]
BATCH_SIZE    = CONFIG["batch_size"]
MAX_LOAD      = CONFIG["max_system_load"]
BODY_PREVIEW  = CONFIG["body_preview_chars"]

# ---------------------------------------------------------------------------
# Regex rules
# ---------------------------------------------------------------------------
_rules = CONFIG.get("regex_rules", {})
SPAM_PATTERNS = [re.compile(p) for p in _rules.get("spam_patterns", [])]
SPAM_THRESHOLD = _rules.get("spam_threshold", 2)

# ---------------------------------------------------------------------------
# LLM config
# ---------------------------------------------------------------------------
_llm = CONFIG.get("llm", {})
LLM_ENABLED = _llm.get("enabled", False)


# ---------------------------------------------------------------------------
# System load
# ---------------------------------------------------------------------------
def current_load() -> float:
    try:
        return float(open("/proc/loadavg").read().split()[0])
    except Exception:
        return 0.0


def should_sleep() -> bool:
    return current_load() > MAX_LOAD


# ---------------------------------------------------------------------------
# Database helpers
# ---------------------------------------------------------------------------
def get_db():
    return mysql.connector.connect(
        host=DB["host"], user=DB["user"], password=DB["password"],
        database=DB["database"], charset="utf8mb4",
        autocommit=False)


# ---------------------------------------------------------------------------
# Lease: claim a batch of unchecked mails atomically
# ---------------------------------------------------------------------------
def claim_batch(cursor) -> list[int]:
    """SELECT FOR UPDATE SKIP LOCKED 原子租赁，返回 mail_id 列表"""
    cursor.execute(
        "SELECT id FROM mails "
        "WHERE spam_status = 0 "
        "ORDER BY send_time ASC "
        "LIMIT %s "
        "FOR UPDATE SKIP LOCKED",
        (BATCH_SIZE,))

    ids = [row[0] for row in cursor.fetchall()]
    if not ids:
        return []

    placeholders = ",".join(["%s"] * len(ids))
    cursor.execute(
        f"UPDATE mails SET spam_status = 3 WHERE id IN ({placeholders})",
        ids)
    return ids


def update_status(cursor, mail_id: int, status: int, score: float = None):
    """更新垃圾检测结果"""
    if score is not None:
        cursor.execute(
            "UPDATE mails SET spam_status = %s, spam_score = %s WHERE id = %s",
            (status, score, mail_id))
    else:
        cursor.execute(
            "UPDATE mails SET spam_status = %s WHERE id = %s",
            (status, mail_id))


# ---------------------------------------------------------------------------
# Detection
# ---------------------------------------------------------------------------
def load_body(body_path: str) -> str:
    """从磁盘加载邮件正文"""
    if not body_path:
        return ""
    abs_path = os.path.join("/opt/smtpServer/mail", body_path)
    if not os.path.isfile(abs_path):
        return ""
    try:
        with open(abs_path, "r", encoding="utf-8", errors="replace") as f:
            return f.read(BODY_PREVIEW)
    except Exception:
        return ""


def regex_detect(subject: str, body: str) -> tuple[bool, float]:
    """正则匹配，返回 (is_spam, confidence)"""
    text = f"{subject}\n{body}"
    hits = sum(1 for p in SPAM_PATTERNS if p.search(text))
    if hits >= SPAM_THRESHOLD:
        confidence = min(0.5 + hits * 0.15, 0.95)
        return True, confidence
    return False, 0.0


def llm_detect(subject: str, body: str) -> tuple[bool, float]:
    """LLM 检测，返回 (is_spam, confidence)"""
    if not LLM_ENABLED:
        return False, 0.0

    prompt = _llm["prompt"].format(
        subject=subject,
        body_preview=body[:BODY_PREVIEW])

    try:
        import urllib.request
        req = urllib.request.Request(
            _llm["api_url"],
            data=json.dumps({
                "model": _llm["model"],
                "messages": [{"role": "user", "content": prompt}],
                "max_tokens": _llm["max_tokens"],
                "temperature": _llm["temperature"],
            }).encode(),
            headers={
                "Content-Type": "application/json",
                "Authorization": f"Bearer {_llm['api_key']}",
            })
        resp = urllib.request.urlopen(req, timeout=_llm["timeout_sec"])
        body_resp = json.loads(resp.read())
        answer = body_resp["choices"][0]["message"]["content"].strip().upper()
        if "SPAM" in answer:
            return True, 0.8
        return False, 0.2
    except Exception as e:
        print(f"[llm] error: {e}")
        return False, 0.0


def detect_one(cursor, mail_id: int):
    """检测一封邮件并更新状态"""
    cursor.execute(
        "SELECT subject, body_path FROM mails WHERE id = %s", (mail_id,))
    row = cursor.fetchone()
    if not row:
        return

    subject, body_path = row
    body = load_body(body_path)

    # Stage 1: 正则快速筛选
    is_spam, conf = regex_detect(subject, body)
    if is_spam:
        update_status(cursor, mail_id, 2, conf)
        return

    # Stage 2: 正则不确定时走 LLM
    if LLM_ENABLED and conf < 0.5:
        is_spam_llm, conf_llm = llm_detect(subject, body)
        if is_spam_llm:
            update_status(cursor, mail_id, 2, conf_llm)
        else:
            update_status(cursor, mail_id, 1, conf_llm)
        return

    # 正常邮件
    update_status(cursor, mail_id, 1, 0.0)


# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------
def run_once():
    db = get_db()
    try:
        cur = db.cursor()
        db.start_transaction()
        mail_ids = claim_batch(cur)
        db.commit()

        if not mail_ids:
            return 0

        print(f"[{datetime.now().strftime('%H:%M:%S')}] claimed {len(mail_ids)} mail(s)")
        for mid in mail_ids:
            try:
                db.start_transaction()
                detect_one(cur, mid)
                db.commit()
            except Exception as e:
                db.rollback()
                print(f"[error] mail {mid}: {e}")

        return len(mail_ids)
    finally:
        db.close()


def main():
    print(f"[spam-detector] starting, batch={BATCH_SIZE}, poll_interval={POLL_INTERVAL}s, max_load={MAX_LOAD}")
    while True:
        if should_sleep():
            load = current_load()
            print(f"[{datetime.now().strftime('%H:%M:%S')}] load={load:.1f} > {MAX_LOAD}, sleeping {POLL_INTERVAL}s")
            time.sleep(POLL_INTERVAL)
            continue

        count = 0
        try:
            count = run_once()
        except mysql.connector.Error as e:
            print(f"[db error] {e}")
        except Exception as e:
            print(f"[error] {e}")

        sleep_time = POLL_INTERVAL / 4 if count == 0 else POLL_INTERVAL
        time.sleep(sleep_time)


if __name__ == "__main__":
    main()
