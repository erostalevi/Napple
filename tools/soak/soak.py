#!/usr/bin/env python3
"""soak.py -- start/soak driver for the Napple runtime: runs a title many times (or in slices), classifies every run and summarises the failure rate.

    soak.py list                                  the known targets (title + build data dirs found on this Mac)
    soak.py run TARGET [--runs N] [--seconds S]   N consecutive starts of S seconds each (default 20 x 45 s)
    soak.py soak TARGET [--minutes M] [--slice S] one long soak as consecutive slices (each slice is a fresh start with the same save root); every run is a separate gamelock.py call
    soak.py summary DIR [DIR...]                  re-summarise one or more result directories (adds up their runs.jsonl)
    add --dry-run to print the commands only

Every game run is wrapped in tools/dev/gamelock.py (the Mac-wide game lock) unless --no-lock, audio is off (NAPPLE_AUDIO_DEVICE=0), and the run is headless unless
--headed. Each run's classification (see classify()):
    ok          exited 0 after the requested time with frames presented and a moving frame counter
    hang        no frames, a frozen frame counter at the end (last two [perf] samples equal), or the wall-clock watchdog had to kill it
    break       guest Break (svc 0x26) -> "FAIL: guest Break"
    segfault    "[napple] CRASH signal=N" (SIGSEGV/SIGBUS/SIGILL/...), or the process died on a signal
    exit        any other exit (non-zero status, early exit, runtime_fail) -- the FAIL line is the signature
Logs of ok runs are deleted; failing runs keep a trimmed log (first 200 + last 400 lines) next to a signature record, in <out>/<target>-<stamp>/.
Result: runs.jsonl (one JSON object per run) and summary.md/summary.json in the same directory. No product code is touched; the runtime binary is an argument."""
import argparse, collections, datetime, glob, json, os, re, shutil, signal, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))                      # the Napple/ folder of the checkout
GAMELOCK = os.path.join(ROOT, "tools", "dev", "gamelock.py")
DATA_ROOT = os.environ.get("NAPPLE_GOLDEN_DATA", "/private/tmp/napple-golden-data")
DEFAULT_RUNTIME = os.path.join(ROOT, "runtime", "build", "napple-runtime")
DEFAULT_OUT = os.environ.get("NAPPLE_SOAK_OUT", "/private/tmp/napple-soak")

# target -> (title id, which version of napple-app.json, script of key presses (NAPPLE_INPUT_SCRIPT) that moves the title along)
TITLES = {
    "las": ("01006BB00C6F0000", "20:A:0.3,24:A:0.3,28:A:0.3,40:A:0.3,50:A:0.3,60:A:0.3,70:A:0.3,80:A:0.3,90:A:0.3"),
    "smo": ("0100000000010000", "30:A:0.3,45:A:0.3,60:A:0.3"),
    "mk8d": ("0100152000022000", ""),
    "acnh": ("01006F8002326000", "20:A:0.3,30:A:0.3,40:A:0.3,50:A:0.3"),
}
# target name -> (title key, version kind). "update"/"base" are the kinds in napple-app.json; the DLC set is the runtime's default for the data dir.
TARGETS = {"las": ("las", "base"), "smo": ("smo", "base"), "mk8d-base": ("mk8d", "base"), "mk8d-update": ("mk8d", "update"),
           "acnh-base": ("acnh", "base"), "acnh-update": ("acnh", "update")}


def data_dir(target):
    key, kind = TARGETS[target]
    tid = TITLES[key][0]
    base = os.path.join(DATA_ROOT, tid)
    app = json.load(open(os.path.join(base, "napple-app.json")))
    for v in app["versions"]:
        if v["kind"] == kind:
            return os.path.join(base, v["dir"]), app
    raise SystemExit("no %s version of %s under %s" % (kind, tid, base))


MARKERS = re.compile(r"CRASH signal|host abort|guest Break|FAIL:|EndRecording without|\[napple\] (esr|pc|lr)=|lr in |unimplemented svc")


def trim(text, head=200, tail=400):
    """Keeps the first/last lines plus a window of 12 lines before and 40 after the first failure marker, so the signature can be re-derived from the kept log."""
    lines = text.splitlines()
    if len(lines) <= head + tail:
        return text
    keep = set(range(head)) | set(range(len(lines) - tail, len(lines)))
    first = next((i for i, l in enumerate(lines) if MARKERS.search(l)), None)
    if first is not None:
        keep |= set(range(max(0, first - 12), min(len(lines), first + 60)))
    out, prev = [], -1
    for i in sorted(keep):
        if i != prev + 1:
            out.append("... %d lines omitted ..." % (i - prev - 1))
        out.append(lines[i])
        prev = i
    return "\n".join(out) + "\n"


def signature(kind, text):
    """A short stable string that groups equal failures, plus the detail lines (pc, callers, last svcs / IPC)."""
    if kind == "break":
        m = re.search(r"guest Break reason=(\S+) result=(\S+) \(module (\d+) desc (\d+)\) from (.+)", text)
        return ("Break reason=%s result=%s from %s" % (m.group(1), m.group(2), m.group(5).strip())) if m else "Break (unparsed)"
    if kind == "segfault":
        m = re.search(r"CRASH signal=(\d+) fault=(\S+) pc=(.+)", text)
        lr = re.search(r"\[napple\] lr in (.+)", text)
        if m:
            fault = int(m.group(2), 16)
            fl = "null+%#x" % fault if fault < 0x10000 else "wild"          # the exact wild address differs every run: group on its class
            return "signal %s fault %s pc %s lr %s" % (m.group(1), fl, m.group(3).strip(), lr.group(1).strip() if lr else "?")
        if "host abort, call stack" in text:
            body = text.split("host abort, call stack:", 1)[1].splitlines()[1:40]
            frames = [re.sub(r"^\d+\s+(\S+)\s+0x\w+\s+", r"\1 ", l).strip() for l in body]
            reason = next((f.split()[1] for f in frames if "libsystem_malloc" in f and ("BUG_IN" in f or "malloc_report" in f)), "")
            first = next((f for f in frames if f.startswith("napple-runtime") and "signals_install" not in f), "?")
            return "host abort (%s) in %s" % (reason or "abort", first.replace("napple-runtime ", ""))
        return "crash (unparsed)"
    if kind == "exit":
        m = re.findall(r"FAIL: (.+)", text)
        return ("FAIL: " + m[-1].strip()) if m else "exit without a FAIL line"
    return ""


def key_lines(kind, text):
    """The lines worth quoting in the stability guide: the failure line, the guest callers that follow it, and the last svc/IPC activity."""
    lines = text.splitlines()
    pat = {"break": r"guest Break", "segfault": r"CRASH signal|\[napple-gfx\] warn: EndRecording|host abort", "exit": r"FAIL:"}.get(kind)
    if not pat:
        return []
    idx = next((i for i, l in enumerate(lines) if re.search(pat, l)), None)
    if idx is None:
        return lines[-12:]
    out = lines[idx:idx + 14]
    out += [l for l in lines[idx:] if re.search(r"svc ring|ipc\b|IPC", l)][:10]
    return [l[:240] for l in out]


def classify(rc, wall, requested, text, killed):
    """Returns (kind, frames, detail). kind in ok/hang/break/segfault/exit."""
    frames = [int(x) for x in re.findall(r"\[perf\].*? frames (\d+)", text)]
    m = re.search(r"stats: frames (\d+)", text)
    total = int(m.group(1)) if m else (frames[-1] if frames else 0)
    if "guest Break" in text and "FAIL: guest Break" in text:
        return "break", total, ""
    if "CRASH signal" in text or "host abort, call stack" in text or (rc is not None and rc < 0 and not killed):
        return "segfault", total, "rc=%s" % rc
    if killed:
        return "hang", total, "watchdog kill after %.0f s" % wall
    if rc != 0:
        return "exit", total, "rc=%s" % rc
    if total == 0:
        return "hang", total, "0 frames presented"
    if len(frames) >= 2 and frames[-1] == frames[-2] and wall > 30:
        return "hang", total, "frame counter frozen at %d over the last perf interval" % frames[-1]
    if wall < requested - 3:
        return "exit", total, "exited 0 after %.0f s of %d" % (wall, requested)
    return "ok", total, ""


def rss_kb(pid):
    try:
        return int(subprocess.check_output(["ps", "-o", "rss=", "-p", str(pid)], stderr=subprocess.DEVNULL).split()[0])
    except Exception:
        return 0


def one_run(a, target, outdir, index, seconds, save_root, extra_env):
    dd, _ = data_dir(target)
    key = TARGETS[target][0]
    env = dict(os.environ)
    env.update({"NAPPLE_AUDIO_DEVICE": "0", "NAPPLE_PERF_LOG": "1", "NAPPLE_SVC_RING": "1", "NAPPLE_TRACE_IPC": a.trace_ipc})
    if not a.no_input and TITLES[key][1]:
        env["NAPPLE_INPUT_SCRIPT"] = TITLES[key][1]
    env.update(extra_env)
    cmd = [a.runtime, dd, "--seconds", str(seconds), "--save-root", save_root]
    if not a.headed:
        cmd.insert(2, "--headless")
    if not a.no_lock:                       # every run is its own gamelock.py call (FIFO): nothing holds the lock between runs
        cmd = [sys.executable, GAMELOCK, "--"] + cmd
    if a.dry_run:
        print("DRY:", " ".join(cmd))
        return None
    log = os.path.join(outdir, "run-%03d.log" % index)
    t_wait = time.time()
    killed = False
    with open(log, "w") as lf:
        # The lock is taken inside gamelock.py, so the watchdog clock only starts once the runtime prints its first line.
        p = subprocess.Popen(cmd, env=env, stdout=lf, stderr=subprocess.STDOUT, cwd=ROOT, start_new_session=True)
        started = None
        peak = 0
        while p.poll() is None:
            time.sleep(2)
            if started is None and os.path.getsize(log) > 0 and "[napple " in open(log, errors="replace").read(4096):
                started = time.time()
            if started is not None:
                peak = max(peak, rss_kb(p.pid))
                if time.time() - started > seconds + a.grace:
                    killed = True
                    os.killpg(p.pid, signal.SIGKILL)
                    p.wait()
        rc = p.returncode
    t_end = time.time()
    wall = t_end - (started or t_wait)
    text = open(log, errors="replace").read()
    kind, frames, detail = classify(rc, wall, seconds, text, killed)
    rec = {"target": target, "index": index, "kind": kind, "rc": rc, "wall_s": round(wall, 1), "requested_s": seconds, "frames": frames,
           "detail": detail, "sig": signature(kind, text), "runtime": a.runtime, "headed": bool(a.headed), "time": int(t_end),
           "locked_wait_s": round((started or t_end) - t_wait - 0, 1)}
    if kind == "ok":
        os.remove(log)
    else:
        rec["key_lines"] = key_lines(kind, text)
        open(log, "w").write(trim(text))
        rec["log"] = os.path.basename(log)
    shutil.rmtree(os.path.join(save_root, TITLES[key][0], "cache"), ignore_errors=True) if a.drop_cache else None
    return rec


def summarise(records):
    by = collections.OrderedDict()
    for r in records:
        by.setdefault(r["target"], []).append(r)
    summ = {}
    for t, rs in by.items():
        c = collections.Counter(r["kind"] for r in rs)
        sigs = collections.Counter((r["kind"], r["sig"]) for r in rs if r["kind"] != "ok")
        summ[t] = {"runs": len(rs), "ok": c["ok"], "hang": c["hang"], "break": c["break"], "segfault": c["segfault"], "exit": c["exit"],
                   "fail_rate": round(1 - c["ok"] / len(rs), 3) if rs else 0, "wall_s_total": round(sum(r["wall_s"] for r in rs)),
                   "signatures": [{"kind": k, "sig": s, "count": n} for (k, s), n in sigs.most_common()]}
    return summ


def write_summary(outdir, records):
    summ = summarise(records)
    json.dump(summ, open(os.path.join(outdir, "summary.json"), "w"), indent=1)
    rows = ["| target | runs | ok | hang | break | segfault | exit | fail rate |", "|---|---|---|---|---|---|---|---|"]
    for t, s in summ.items():
        rows.append("| %s | %d | %d | %d | %d | %d | %d | %.0f%% |" % (t, s["runs"], s["ok"], s["hang"], s["break"], s["segfault"], s["exit"], s["fail_rate"] * 100))
    sig = ["", "Signatures:"]
    for t, s in summ.items():
        for g in s["signatures"]:
            sig.append("- %s: %dx %s `%s`" % (t, g["count"], g["kind"], g["sig"]))
    text = "\n".join(rows + sig) + "\n"
    open(os.path.join(outdir, "summary.md"), "w").write(text)
    return text


def load(paths):
    recs = []
    for p in paths:
        f = os.path.join(p, "runs.jsonl")
        if os.path.exists(f):
            recs += [json.loads(l) for l in open(f) if l.strip()]
    return recs


def make_outdir(a, target, what):
    d = os.path.join(a.out, "%s-%s-%s" % (target, what, datetime.datetime.now().strftime("%Y%m%d-%H%M%S")))
    if not a.dry_run:
        os.makedirs(d, exist_ok=True)
    return d


def run_series(a, target, n, seconds, what):
    out = make_outdir(a, target, what)
    save = os.path.join(out, "save")
    recs = []
    for i in range(1, n + 1):
        rec = one_run(a, target, out, i, seconds, save, {})
        if rec is None:
            continue
        recs.append(rec)
        with open(os.path.join(out, "runs.jsonl"), "a") as f:
            f.write(json.dumps(rec) + "\n")
        print("[%s %d/%d] %s frames=%d wall=%.0fs %s" % (target, i, n, rec["kind"], rec["frames"], rec["wall_s"], rec["sig"]), flush=True)
        if a.stop_after and sum(r["kind"] != "ok" for r in recs) >= a.stop_after:
            break
    if recs:
        shutil.rmtree(save, ignore_errors=True)
        print(write_summary(out, recs))
        print("results:", out)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("cmd", choices=["list", "run", "soak", "summary"])
    ap.add_argument("target", nargs="*", help="run/soak: " + ", ".join(TARGETS) + "; summary: result directories")
    ap.add_argument("--runs", type=int, default=20)
    ap.add_argument("--seconds", type=int, default=45, help="seconds per run (the runtime's --seconds)")
    ap.add_argument("--minutes", type=float, default=20, help="soak: total runtime minutes across the slices")
    ap.add_argument("--slice", type=int, default=170, help="soak: seconds per locked slice (keep <= 180 so other sessions get their turn)")
    ap.add_argument("--headed", action="store_true", help="open the game window instead of running headless")
    ap.add_argument("--runtime", default=DEFAULT_RUNTIME)
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--grace", type=int, default=45, help="watchdog: seconds past the requested time before a run counts as hung and is killed")
    ap.add_argument("--trace-ipc", default="1", help="NAPPLE_TRACE_IPC value (1 = error results only, all = every command; huge logs)")
    ap.add_argument("--no-lock", action="store_true", help="do not take the game lock (only when no other session uses the Mac)")
    ap.add_argument("--no-input", action="store_true", help="do not send the scripted key presses that move the title along")
    ap.add_argument("--stop-after", type=int, default=0, help="stop a series after this many failures")
    ap.add_argument("--drop-cache", action="store_true", help="delete the shader cache between runs")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    if a.cmd == "list":
        for t in TARGETS:
            try:
                d, _ = data_dir(t)
                print("%-12s %s %s" % (t, d, "ok" if os.path.isdir(d) else "MISSING"))
            except Exception as e:
                print("%-12s unavailable (%s)" % (t, e))
        return
    if a.cmd == "summary":
        recs = load(a.target)
        for d in a.target:                                   # re-derive the signature of kept failing logs with the current rules
            for r in recs:
                lf = os.path.join(d, r.get("log", "")) if r.get("log") else None
                if lf and os.path.exists(lf) and r["kind"] != "ok":
                    ns = signature(r["kind"], open(lf, errors="replace").read())
                    if "unparsed" not in ns:
                        r["sig"] = ns
        print(write_summary(a.target[0], recs) if recs else "no runs.jsonl found")
        return
    if len(a.target) != 1 or a.target[0] not in TARGETS:
        ap.error("give one target: " + ", ".join(TARGETS))
    t = a.target[0]
    if a.cmd == "run":
        run_series(a, t, a.runs, a.seconds, "runs")
    else:
        n = max(1, int(round(a.minutes * 60 / a.slice)))
        run_series(a, t, n, a.slice, "soak")


if __name__ == "__main__":
    main()
