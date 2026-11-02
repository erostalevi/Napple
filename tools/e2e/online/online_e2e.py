#!/usr/bin/env python3
"""Napple Online end-to-end (item online-tieup): account -> second device -> profile/photo/tier -> cloud save -> friends -> tier matrix.

Drives the real client core (crates/napple-cloud example `online_cli`, the same code the overlay's table uses) against a local stack
(infra/local/run.sh start) or the live servers. Prints a PASS/FAIL table; exit 1 on any FAIL.

  tools/e2e/online/online_e2e.py [--live] [--keep]
Env: NAPPLE_ACCOUNT_URL / NAPPLE_SOCIAL_URL (defaults: local stack ports), E2E_MAIL_LOG (local dev mailer log; default the stack's account.log),
     E2E_CODE_CMD (live: a command that prints the newest 6-digit code mailed to the address given as $1).
"""
import json, os, shutil, struct, subprocess, sys, tempfile, time, urllib.request, zlib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../.."))
CLI = os.path.join(ROOT, "target/debug/examples/online_cli")
LIVE = "--live" in sys.argv
STACK = os.path.join(os.environ.get("TMPDIR", "/tmp"), "napple-local-stack")
ACC = os.environ.get("NAPPLE_ACCOUNT_URL", "https://pretend.napple.lat" if LIVE else "http://127.0.0.1:18082")
SOC = os.environ.get("NAPPLE_SOCIAL_URL", "https://pretend.napple.lat" if LIVE else "http://127.0.0.1:18083")
MAILLOG = os.environ.get("E2E_MAIL_LOG", os.path.join(STACK, "logs/account.log"))
TMP = tempfile.mkdtemp(prefix="napple-e2e-")
rows = []

def check(name, ok, evidence=""):
    rows.append((name, bool(ok), str(evidence)[:160]))
    print(("PASS " if ok else "FAIL ") + name + ("  | " + str(evidence)[:160] if evidence else ""), flush=True)
    return ok

def env(home):
    e = dict(os.environ, NAPPLE_HOME=home, NAPPLE_ACCOUNT_STORE="file", NAPPLE_ACCOUNT_URL=ACC, NAPPLE_SOCIAL_URL=SOC)
    return e

def cli(home, *a):
    r = subprocess.run([CLI, *a], env=env(home), capture_output=True, text=True, timeout=120)
    return r.stdout.strip() + (("\n" + r.stderr.strip()) if r.returncode else "")

def act(home, action, **args):
    out = cli(home, "act", action, json.dumps(args))
    code, _, js = out.partition(" ")
    try:
        return int(code), json.loads(js)
    except Exception:
        return -1, {"raw": out}

def state(home):
    return json.loads(cli(home, "state"))

def mail_code(email):
    if os.environ.get("E2E_CODE_DIR"):  # live: whoever reads the mailbox drops the code into $E2E_CODE_DIR/<address> (the newest wins; the file is consumed)
        f = os.path.join(os.environ["E2E_CODE_DIR"], email)
        print("WAITING-CODE " + email, flush=True)
        for _ in range(240):
            if os.path.exists(f):
                c = open(f).read().strip(); os.remove(f); return c
            time.sleep(1)
        return ""
    if os.environ.get("E2E_CODE_CMD"):
        for _ in range(30):
            out = subprocess.run(os.environ["E2E_CODE_CMD"], shell=True, input=email, capture_output=True, text=True).stdout.strip()
            if out: return out
            time.sleep(4)
        return ""
    for _ in range(20):
        txt = open(MAILLOG).read() if os.path.exists(MAILLOG) else ""
        parts = txt.split("To: " + email)
        if len(parts) > 1:
            import re
            m = re.search(r"^\s{2,}(\d{6})\s*$", parts[-1], re.M)
            if m: return m.group(1)
        time.sleep(0.5)
    return ""

def png(path):
    w = h = 64
    raw = b"".join(b"\x00" + bytes([x * 4 % 256, y * 4 % 256, 128] * 1) * 1 for y in range(h) for x in [0])  # placeholder, replaced below
    rows_ = b"".join(b"\x00" + b"".join(bytes([x * 4 % 256, y * 4 % 256, 128]) for x in range(w)) for y in range(h))
    def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(rows_)) + chunk(b"IEND", b""))

def http(method, url, token, profile=None, body=None):
    req = urllib.request.Request(url, method=method, data=json.dumps(body).encode() if body is not None else None)
    req.add_header("Authorization", "Bearer " + token)
    if profile: req.add_header("X-Napple-Profile", profile)
    if body is not None: req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=20) as r: return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        return e.code, {}

def main():
    tag = str(int(time.time()))[-6:]
    base = os.environ.get("E2E_EMAIL_BASE", "tieup")  # live: local part before +tag@domain
    dom = os.environ.get("E2E_EMAIL_DOMAIN", "napple.lat" if LIVE else "example.com")
    def addr(n): return f"{base}-{n}{tag}@{dom}"
    users = {}
    for n in ("a", "b", "c"):
        users[n] = dict(email=addr(n), nick=f"Tieup{n.upper()}{tag}", pw="correct horse battery 1", home=os.path.join(TMP, "home-" + n))
    # 1 sign-up + verify
    for n, u in users.items():
        rc, js = act(u["home"], "signup", email=u["email"], password=u["pw"], nickname=u["nick"], age_ok=True, terms_ok=True)
        if not check(f"signup {n}", rc == 0, js): return
        rc, js = act(u["home"], "login", email=u["email"], password=u["pw"])
        check(f"login before verify is refused (unverified) {n}", rc == 3, js)
        code = mail_code(u["email"])
        if not check(f"6-digit code mail received {n}", len(code) == 6, code and "got a code"): return
        rc, js = act(u["home"], "verify", email=u["email"], code=code)
        check(f"verify {n} -> tokens", rc == 0, js)
        st = state(u["home"])
        check(f"first profile created with signup nickname {n}", st.get("signed_in") and st["current"]["nickname"] == u["nick"], st.get("current", {}).get("nickname"))
        check(f"E2E save key made on first device {n}", st.get("has_account_key"))
        creds = os.path.join(u["home"], "online/credentials.json")
        check(f"credentials file 0600, no plain token {n}", oct(os.stat(creds).st_mode & 0o777) == "0o600" and "refresh_token\"" not in open(creds).read())
    A, B, C = users["a"], users["b"], users["c"]
    # 2 second device of A
    A2 = os.path.join(TMP, "home-a2")
    rc, js = act(A2, "login", email=A["email"], password=A["pw"])
    check("second device password login", rc == 0, js)
    st2 = state(A2)
    check("second device has no save key yet", st2.get("signed_in") and not st2.get("has_account_key"))
    sa = state(A["home"])
    pend = [d for d in sa["devices"] if d["pending"]]
    check("device A sees the pending approval", len(pend) == 1, [d["name"] for d in pend])
    if pend:
        rc, js = act(A["home"], "approve_device", device_id=pend[0]["id"])
        check("approve_device", rc == 0, js)
    st2 = state(A2)
    check("second device received the save key", st2.get("has_account_key"))
    # 3 profile, photo, tier
    pid = sa["current"]["id"]
    rc, js = act(A["home"], "update_profile", profile_id=pid, profile={"bio": "e2e bio", "icon_id": 7, "links": [{"kind": "web", "value": "https://napple.lat"}]})
    check("update_profile (bio/icon/https link)", rc == 0, js)
    rc, js = act(A["home"], "update_profile", profile_id=pid, profile={"links": [{"kind": "web", "value": "http://insecure.example"}]})
    check("non-https web link is rejected", rc == 2, js)
    p = os.path.join(TMP, "photo.png"); png(p)
    rc, js = act(A["home"], "upload_photo", profile_id=pid, path=p)
    check("upload_photo (PNG)", rc == 0, js)
    junk = os.path.join(TMP, "junk.png"); open(junk, "wb").write(b"not an image" * 10)
    rc, js = act(A["home"], "upload_photo", profile_id=pid, path=junk)
    check("junk photo refused by magic bytes (422 -> rejected)", rc == 2, js)
    st = state(A["home"])
    check("profile edits visible in state", st["current"]["bio"] == "e2e bio" and st["current"]["icon_id"] == 7, st["current"]["bio"])
    check("photo cached locally as 256x256 JPEG", st["current"]["photo_path"] and open(st["current"]["photo_path"], "rb").read(2) == b"\xff\xd8", st["current"]["photo_path"])
    rc, js = act(A["home"], "set_tier", profile_id=pid, tier=2)
    check("set_tier 2", rc == 0, js)
    act(A["home"], "set_tier", profile_id=pid, tier=1)
    # 4 cloud saves: device A writes, device B(A2) pulls before mount
    title = "0100152000022000"; uid = "4e6170706c6555736572000000000001"
    sup_a, sup_a2 = os.path.join(TMP, "sup-a"), os.path.join(TMP, "sup-a2")
    sd_a, sd_a2 = os.path.join(TMP, "save-a"), os.path.join(TMP, "save-a2")
    d = os.path.join(sd_a, "space1/account", uid[:8]); os.makedirs(d)
    open(os.path.join(d, "save.dat"), "w").write("e2e save payload " + tag)
    for s in (sup_a, sup_a2, sd_a2): os.makedirs(s, exist_ok=True)
    out = cli(A["home"], "cloud", sup_a, sd_a, title, uid, "push")
    check("cloud save pushed from device A", "flush 1" in out and '"status":"synced"' in out.replace(" ", ""), out[-160:])
    out = cli(A2, "cloud", sup_a2, sd_a2, title, uid)
    got = os.path.join(sd_a2, "space1/account", uid[:8], "save.dat")
    check("device B pulled the save before mount", os.path.exists(got) and open(got).read() == "e2e save payload " + tag, out[-160:])
    # 5 friends A <-> B by code
    code_a = state(A["home"])["current"]["friend_code"]
    check("friend code format NP-…", code_a.startswith("NP-"), code_a)
    rc, js = act(B["home"], "friend_request", code=code_a)
    check("B sends a friend request by A's code", rc == 0, js)
    sa = state(A["home"])
    check("A sees the incoming request", any(r["incoming"] for r in sa["requests"]), len(sa["requests"]))
    bid = state(B["home"])["current"]["id"]
    rc, js = act(A["home"], "friend_accept", profile_id=bid)
    check("A accepts", rc == 0, js)
    check("both lists show the friend", any(f["profile_id"] == bid for f in state(A["home"])["friends"]) and any(f["profile_id"] == pid for f in state(B["home"])["friends"]))
    # 6 tier matrix: B's tier vs stranger C (search / request) and friend A (presence)
    tok_b = cli(B["home"], "token")
    cid = state(C["home"])["current"]["id"]
    code_b = state(B["home"])["current"]["friend_code"]
    http("PUT", SOC + "/social/v1/presence", tok_b, bid, {"status": "playing", "title_id": title, "title_name": "Mario Kart 8 Deluxe", "mode": "online"})
    exp = {1: (True, True, True), 2: (True, True, True), 3: (False, False, True), 4: (False, False, True), 5: (False, False, False)}
    for tier, (search, req, pres) in exp.items():
        act(B["home"], "set_tier", profile_id=bid, tier=tier)
        time.sleep(float(os.environ.get('E2E_TIER_WAIT', '11' if LIVE else '2.5')))  # napple-social trusts a profile snapshot for NAPPLE_SOCIAL_PROFILE_TTL_SECS (10 s; the local stack runs with 2)
        # reset any request/friendship state C has with B
        rc, js = act(C["home"], "search", query=B["nick"])
        found = any(r["profile_id"] == bid for r in js.get("results", []))
        rc2, js2 = act(C["home"], "search", query=code_b)
        found_code = any(r["profile_id"] == bid for r in js2.get("results", []))
        check(f"tier {tier}: stranger search finds B = {search}", found == search and found_code == search, (found, found_code))
        rc, js = act(C["home"], "friend_request", code=code_b)
        sent = rc == 0
        check(f"tier {tier}: stranger friend request by code allowed = {req}", sent == req, (rc, js.get("reason")))
        if sent:
            act(B["home"], "friend_decline", profile_id=cid)
        fr = [f for f in state(A["home"])["friends"] if f["profile_id"] == bid]
        online = bool(fr and fr[0]["online"])
        check(f"tier {tier}: friend A sees B's presence = {pres}", online == pres, fr and (fr[0]["online"], fr[0]["title_id"]))
    act(B["home"], "set_tier", profile_id=bid, tier=1)
    # 7 logout / delete
    rc, js = act(A2, "logout")
    check("logout on device 2", rc == 0 and not state(A2).get("signed_in"))
    # 8 account deletion (throwaway accounts: also the cleanup)
    for n, u in users.items():
        rc, js = act(u["home"], "delete_account", password=u["pw"])
        check(f"delete_account {n}", rc == 0 and not state(u["home"]).get("signed_in"), js)
    rc, js = act(A["home"], "login", email=A["email"], password=A["pw"])
    check("deleted account can no longer sign in", rc != 0, js)
    print("\n=== %d/%d passed ===" % (sum(1 for r in rows if r[1]), len(rows)))
    open(os.path.join(TMP, "results.json"), "w").write(json.dumps(rows))

if __name__ == "__main__":
    if not os.path.exists(CLI): sys.exit("build first: cargo build -p napple-cloud --example online_cli")
    try: main()
    finally:
        print("work dir:", TMP)
    sys.exit(0 if all(r[1] for r in rows) else 1)
