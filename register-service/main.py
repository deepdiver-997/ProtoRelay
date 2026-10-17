#!/usr/bin/env python3
"""
ProtoRelay 邀请码注册服务
启动: uvicorn main:app --host 127.0.0.1 --port 8080
前面用 nginx/Caddy 反代并加 SSL，或直接用 SSH 本地转发访问。
"""

import json, re, time, os, subprocess, hmac
from datetime import datetime, timedelta
from typing import Optional

import mysql.connector
from fastapi import FastAPI, HTTPException, Header, Request
from pydantic import BaseModel

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(BASE_DIR, "config.json")) as f:
    CONFIG = json.load(f)

DB_CONFIG = {
    "host": CONFIG["db_host"],
    "user": CONFIG["db_user"],
    "password": CONFIG["db_password"],
    "database": CONFIG["db_database"],
    "charset": "utf8mb4",
}

# C++ bcrypt tool (must match server's bcrypt implementation)
HASH_TOOL = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "hash_tool")

def bcrypt_hash(password: str) -> str:
    """使用 C++ hash_tool 生成与服务器完全兼容的 bcrypt hash"""
    result = subprocess.run([HASH_TOOL, password], capture_output=True, text=True, timeout=5)
    if result.returncode != 0:
        raise RuntimeError(f"hash_tool failed: {result.stderr}")
    return result.stdout.strip()

app = FastAPI(title="ProtoRelay Register", version="1.0")

# ---------------------------------------------------------------------------
# Rate limiter (in-memory, resets on restart — enough for personal use)
# ---------------------------------------------------------------------------
_rate_buckets: dict[str, list[float]] = {}

def check_rate_limit(ip: str) -> bool:
    now = time.time()
    window = CONFIG["rate_limit_window_hours"] * 3600
    max_req = CONFIG["rate_limit_per_ip"]
    bucket = _rate_buckets.get(ip, [])
    bucket = [t for t in bucket if now - t < window]
    if len(bucket) >= max_req:
        return False
    bucket.append(now)
    _rate_buckets[ip] = bucket
    return True

# ---------------------------------------------------------------------------
# DB helpers
# ---------------------------------------------------------------------------
def get_db():
    return mysql.connector.connect(**DB_CONFIG)

# ---------------------------------------------------------------------------
# Models
# ---------------------------------------------------------------------------
class RegisterRequest(BaseModel):
    invite_code: str = ""
    password: str = ""

class StatsResponse(BaseModel):
    invite_code: str
    used: int
    max_uses: int
    remaining: int

# ---------------------------------------------------------------------------
# Routes
# ---------------------------------------------------------------------------
@app.get("/")
def root():
    return {"service": "ProtoRelay Invite Registration", "version": "1.0"}

@app.get("/health")
def health():
    return {"status": "ok"}

@app.get("/stats/{code}")
def stats(code: str):
    """查询邀请码使用情况"""
    db = get_db()
    try:
        cur = db.cursor(dictionary=True)
        cur.execute(
            "SELECT code, max_uses, used_count, is_active "
            "FROM invite_codes WHERE code = %s", (code,))
        row = cur.fetchone()
        if not row:
            raise HTTPException(404, "Invite code not found")
        return StatsResponse(
            invite_code=row["code"],
            used=row["used_count"],
            max_uses=row["max_uses"],
            remaining=row["max_uses"] - row["used_count"],
        )
    finally:
        db.close()

@app.post("/register")
def register(req: RegisterRequest, request: Request):
    """邀请码注册: POST /register { invite_code, password }"""
    # IP rate limit
    ip = request.client.host if request.client else "unknown"
    if not check_rate_limit(ip):
        raise HTTPException(429, "Too many attempts, try again later")

    # Validation
    code = req.invite_code.strip()
    pwd = req.password
    if not code or not pwd:
        raise HTTPException(400, "invite_code and password required")
    if len(pwd) < 4:
        raise HTTPException(400, "Password must be at least 4 characters")

    # Max email length check (invitor_999999@<DOMAIN> ≈ 30 chars, safe)
    if len(code) > 64:
        raise HTTPException(400, "Invite code too long")

    db = get_db()
    try:
        cur = db.cursor(dictionary=True)

        # Lookup invite code
        cur.execute(
            "SELECT id, max_uses, used_count, expires_days, is_active "
            "FROM invite_codes WHERE code = %s FOR UPDATE", (code,))
        row = cur.fetchone()
        if not row:
            raise HTTPException(400, "Invalid invite code")
        if not row["is_active"]:
            raise HTTPException(400, "Invite code is no longer active")
        if row["used_count"] >= row["max_uses"]:
            raise HTTPException(400, "Invite code has been fully used")

        # Assign sequential email: invitor_N@<register_domain>（域名来自 config.json）
        seq = row["used_count"] + 1
        email = f"invitor_{seq}@{CONFIG.get('register_domain', 'example.com')}"

        # Check email uniqueness
        cur.execute("SELECT id FROM users WHERE mail_address = %s", (email,))
        if cur.fetchone():
            raise HTTPException(409, f"Email {email} already exists — this should not happen")

        # bcrypt hash via C++ tool (compatible with server)
        hashed = bcrypt_hash(pwd)

        # ------- Insert user + update counters atomically
        cur.execute(
            "INSERT INTO users (mail_address, password, name) VALUES (%s, %s, %s)",
            (email, hashed, f"Invitor #{seq}"))
        user_id = cur.lastrowid

        # Trigger auto-creates mailboxes; verify
        cur.execute("SELECT COUNT(*) AS cnt FROM mailboxes WHERE user_id = %s", (user_id,))
        if cur.fetchone()["cnt"] == 0:
            # Manual fallback
            for box_name in ["收件箱", "发件箱", "垃圾箱", "草稿箱", "已删除"]:
                cur.execute(
                    "INSERT INTO mailboxes (user_id, name) VALUES (%s, %s)",
                    (user_id, box_name))

        # Calculate expiry
        expires_at = datetime.now() + timedelta(days=row["expires_days"])

        # Insert registration record
        cur.execute(
            "INSERT INTO invite_registrations "
            "(user_id, code_id, invite_code, seq_num, email, expires_at) "
            "VALUES (%s, %s, %s, %s, %s, %s)",
            (user_id, row["id"], code, seq, email, expires_at))

        # Increment used_count
        cur.execute(
            "UPDATE invite_codes SET used_count = used_count + 1 WHERE id = %s",
            (row["id"],))

        db.commit()

        return {
            "success": True,
            "email": email,
            "expires_at": expires_at.isoformat(),
            "note": f"Account expires after {row['expires_days']} days",
        }
    except HTTPException:
        db.rollback()
        raise
    except Exception as e:
        db.rollback()
        raise HTTPException(500, f"Registration failed: {e}")
    finally:
        db.close()

# ---------------------------------------------------------------------------
# Admin 管理后台（webmail kickoff MVP-1：别名 / 账号状态 / 发信配额）
#
# 鉴权：X-Admin-Key 头与 config.json 的 admin_key 常量时间比对；config 未配
# admin_key（空）→ 一律 403 fail-closed。
#
# 别名（mail_aliases：alias→target 多行 = 免费邮件组）写入侧防链：
#   - target 必须是 users 表在用账号；
#   - alias 地址不得与任何用户地址重合（两命名空间分离）；
#   - target 不得本身是别名（禁 alias→alias 链）。
# 与 C++ 读侧兜底（build_alias_targets_query 的 JOIN users）双保险。
# 注意：SMTP 进程内别名缓存 TTL 300s，改别名后最多 5 分钟才影响 RCPT 校验。
# ---------------------------------------------------------------------------

EMAIL_RE = re.compile(r"^[^@\s,;:<>]+@[^@\s,;:<>]+\.[^@\s,;:<>]+$")

class AliasCreate(BaseModel):
    alias_address: str
    target_address: str

class StatusUpdate(BaseModel):
    status: int  # 1=启用 0=禁用

def require_admin(x_admin_key: str = Header(default="")):
    key = str(CONFIG.get("admin_key", ""))
    if not key or not hmac.compare_digest(x_admin_key, key):
        raise HTTPException(401, "admin key required")

def _valid_address(addr: str) -> bool:
    return bool(addr) and len(addr) <= 255 and bool(EMAIL_RE.match(addr))

@app.get("/admin/users")
def admin_users(x_admin_key: str = Header(default=""), limit: int = 50, offset: int = 0):
    """用户列表（id/地址/状态/当日配额计数）"""
    require_admin(x_admin_key)
    limit = max(1, min(limit, 500))
    offset = max(0, offset)
    db = get_db()
    try:
        cur = db.cursor(dictionary=True)
        cur.execute(
            "SELECT id, mail_address, name, status, sent_today, sent_date, register_time "
            "FROM users ORDER BY id LIMIT %s OFFSET %s", (limit, offset))
        return {"users": cur.fetchall()}
    finally:
        db.close()

@app.post("/admin/users/{user_id}/status")
def admin_user_status(user_id: int, req: StatusUpdate,
                      x_admin_key: str = Header(default="")):
    """启用/禁用账号（禁用后三协议 + webmail 登录同时拒绝，受各自缓存 TTL 影响）"""
    require_admin(x_admin_key)
    if req.status not in (0, 1):
        raise HTTPException(400, "status must be 0 or 1")
    db = get_db()
    try:
        cur = db.cursor(dictionary=True)
        # 先查存在再更新：MySQL 默认 rowcount 是"改变的行数"，幂等设置
        # （状态不变）会得 0，不能拿来判存在性
        cur.execute("SELECT id FROM users WHERE id = %s", (user_id,))
        if not cur.fetchone():
            raise HTTPException(404, "user not found")
        cur.execute("UPDATE users SET status = %s WHERE id = %s", (req.status, user_id))
        db.commit()
        return {"ok": True, "user_id": user_id, "status": req.status}
    except HTTPException:
        db.rollback()
        raise
    finally:
        db.close()

@app.post("/admin/users/{user_id}/reset-quota")
def admin_user_reset_quota(user_id: int, x_admin_key: str = Header(default="")):
    """重置当日发信计数（解限今天已发满的用户；sent_date 置 NULL = 首封按 0 计）"""
    require_admin(x_admin_key)
    db = get_db()
    try:
        cur = db.cursor(dictionary=True)
        cur.execute("SELECT id FROM users WHERE id = %s", (user_id,))
        if not cur.fetchone():
            raise HTTPException(404, "user not found")
        cur.execute(
            "UPDATE users SET sent_today = 0, sent_date = NULL WHERE id = %s", (user_id,))
        db.commit()
        return {"ok": True, "user_id": user_id}
    except HTTPException:
        db.rollback()
        raise
    finally:
        db.close()

@app.get("/admin/aliases")
def admin_alias_list(alias: str, x_admin_key: str = Header(default="")):
    """查一个别名/组地址的全部投递目标"""
    require_admin(x_admin_key)
    if not _valid_address(alias):
        raise HTTPException(400, "invalid alias address")
    db = get_db()
    try:
        cur = db.cursor(dictionary=True)
        cur.execute(
            "SELECT target_address, is_active, created_at FROM mail_aliases "
            "WHERE alias_address = %s ORDER BY id", (alias,))
        return {"alias": alias, "targets": cur.fetchall()}
    finally:
        db.close()

@app.post("/admin/aliases")
def admin_alias_create(req: AliasCreate, x_admin_key: str = Header(default="")):
    """加一条 alias→target（同别名多行 = 邮件组成员追加，幂等）"""
    require_admin(x_admin_key)
    alias, target = req.alias_address.strip(), req.target_address.strip()
    if not _valid_address(alias) or not _valid_address(target):
        raise HTTPException(400, "invalid address format")

    db = get_db()
    try:
        cur = db.cursor(dictionary=True)
        # 防链 1：target 必须是在用真实账号
        cur.execute("SELECT id, status FROM users WHERE mail_address = %s", (target,))
        row = cur.fetchone()
        if not row:
            raise HTTPException(400, f"target {target} is not an existing user")
        if row["status"] != 1:
            raise HTTPException(400, f"target {target} is disabled")
        # 防链 2：alias 地址不得与用户地址重合（命名空间分离）
        cur.execute("SELECT id FROM users WHERE mail_address = %s", (alias,))
        if cur.fetchone():
            raise HTTPException(400, f"alias {alias} collides with an existing user address")
        # 防链 3：target 不得本身是别名（alias→alias 链，读侧 JOIN users 亦兜底）
        cur.execute("SELECT id FROM mail_aliases WHERE alias_address = %s", (target,))
        if cur.fetchone():
            raise HTTPException(400, f"target {target} is itself an alias (chains forbidden)")

        cur.execute(
            "INSERT INTO mail_aliases (alias_address, target_address) VALUES (%s, %s) "
            "ON DUPLICATE KEY UPDATE is_active = 1", (alias, target))
        db.commit()
        return {"ok": True, "alias": alias, "target": target,
                "note": "SMTP RCPT 展开受 5 分钟别名缓存影响"}
    except HTTPException:
        db.rollback()
        raise
    except Exception as e:
        db.rollback()
        raise HTTPException(500, f"alias create failed: {e}")
    finally:
        db.close()

@app.delete("/admin/aliases/{alias_address}")
def admin_alias_delete(alias_address: str, x_admin_key: str = Header(default=""),
                       target: Optional[str] = None):
    """删别名：不给 target 删整组；给了只删一行"""
    require_admin(x_admin_key)
    if not _valid_address(alias_address):
        raise HTTPException(400, "invalid alias address")
    if target is not None and not _valid_address(target):
        raise HTTPException(400, "invalid target address")
    db = get_db()
    try:
        cur = db.cursor()
        if target is None:
            cur.execute("DELETE FROM mail_aliases WHERE alias_address = %s", (alias_address,))
        else:
            cur.execute(
                "DELETE FROM mail_aliases WHERE alias_address = %s AND target_address = %s",
                (alias_address, target))
        db.commit()
        if cur.rowcount == 0:
            raise HTTPException(404, "alias row not found")
        return {"ok": True, "removed": cur.rowcount}
    except HTTPException:
        db.rollback()
        raise
    finally:
        db.close()

# ---------------------------------------------------------------------------
# Bootstrap: ensure invite codes exist in DB
# ---------------------------------------------------------------------------
def bootstrap_invite_codes():
    db = get_db()
    try:
        cur = db.cursor()
        for ic in CONFIG.get("invite_codes", []):
            cur.execute(
                "INSERT IGNORE INTO invite_codes (code, max_uses, expires_days) "
                "VALUES (%s, %s, %s)",
                (ic["code"], ic["max_uses"], ic.get("expires_days", 90)))
        db.commit()
    finally:
        db.close()

bootstrap_invite_codes()
print("Invite codes bootstrapped:", [ic["code"] for ic in CONFIG.get("invite_codes", [])])

# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    import uvicorn
    uvicorn.run("main:app", host=CONFIG["listen_host"], port=CONFIG["listen_port"], reload=True)
