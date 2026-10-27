#!/usr/bin/env python3
"""golden.py - golden-frame harness for the graphics regression gate (docs/guides/testing-graphics.md).

  golden.py build   <title|all> [--data-root DIR]       build the title's data (napple build --data-only --update-romfs in-place)
  golden.py run     <title|all> [--route R] [options]   run the scripted routes headless, capture frames, scan the logs, diff against the goldens
  golden.py diff    <run-dir>                           re-diff a finished run against the goldens
  golden.py sheet   <run-dir>                           (re)write the contact sheet (contact.html) of a run
  golden.py approve <run-dir> [--title T] [--route R]   copy the run's frames into the golden set (after you looked at the contact sheet)

Goldens are game screenshots: they live in --golden-root (default ~/Library/Application Support/Napple/goldens), never in the repository; the repository keeps
only routes.json (routes, frame numbers, tolerances). A run writes <out>/<stamp>/<title>/<route>/{frame-N.png, log.txt, result.json} and <out>/<stamp>/report.json,
contact.html. Exit status: 0 = every route passed (no gfx warning, no Metal validation message, no crash, every frame within tolerance of its golden; frames
without a golden are reported as 'new' and do not fail unless --strict), 1 = a failure.
"""
import argparse, datetime, glob, html, json, os, re, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))                    # the Napple/ repository root
MANIFEST = json.load(open(os.path.join(HERE, "routes.json")))
DEFAULT_GOLDEN = os.path.expanduser("~/Library/Application Support/Napple/goldens")
TESTING = "/Users/neto/Documents/Napple/Testing Material"


def titles_for(sel):
    t = MANIFEST["titles"]
    if sel == "all":
        return list(t)
    if sel not in t:
        sys.exit("unknown title %r (have: %s)" % (sel, ", ".join(t)))
    return [sel]


def data_dir(root, tid):
    """The newest build directory of a title under <root>/<title id>/ (updates sort last by mtime of their manifest)."""
    base = os.path.join(root, tid)
    cands = [d for d in glob.glob(os.path.join(base, "*")) if os.path.isdir(d) and os.path.exists(os.path.join(d, "manifest.json"))]
    if not cands:
        sys.exit("no data directory under %s: run `golden.py build`" % base)
    app = os.path.join(base, "napple-app.json")
    if os.path.exists(app):
        try:
            want = json.load(open(app))["defaults"]["version"]
            if os.path.isdir(os.path.join(base, want)):
                return os.path.join(base, want)
        except Exception:
            pass
    return max(cands, key=os.path.getmtime)


def cmd_build(a):
    cli = a.napple or os.path.join(ROOT, "target/release/napple")
    if not os.path.exists(cli):
        cli = shutil.which("napple") or sys.exit("no napple CLI: pass --napple or build it (cargo build --release -p napple-cli)")
    for t in titles_for(a.title):
        spec = MANIFEST["titles"][t]
        srcs = [os.path.join(a.testing, n) for n in spec["nsp"]]
        out = a.data_root   # napple build writes <out>/<title id>/<build id>/, where cmd_run (data_dir) looks
        os.makedirs(out, exist_ok=True)
        cmd = [cli, "build"] + srcs + ["-k", os.path.join(a.testing, "Keys 22.1.0/prod.keys"), "--firmware", os.path.join(a.testing, "Firmware.22.1.0.zip"),
               "--updates", "latest", "--dlc", "all", "--data-only", "--update-romfs", "in-place", "-o", out]
        print("+", " ".join(cmd))
        subprocess.check_call(cmd)


def scan_log(text):
    """(gfx warnings, validation messages) found in a run's log."""
    warn, val = {}, {}
    for line in text.splitlines():
        if re.search(r"\] (warn|error):", line):                # only warning / error lines (the summary lines say "0 failed", "unimplemented calls 0")
            for p in MANIFEST["warning_patterns"]:
                if re.search(p, line):
                    warn.setdefault(p, []).append(line.strip()[:240])
        if "Validation Enabled" in line:
            continue
        for p in MANIFEST["validation_patterns"]:
            if p in line:
                val.setdefault(p, []).append(line.strip()[:240])
    return warn, val


def compare(a_path, b_path, tol):
    from PIL import Image, ImageChops
    A = Image.open(a_path).convert("RGB"); B = Image.open(b_path).convert("RGB")
    if A.size != B.size:
        B = B.resize(A.size)
    sa, sb = A.resize((64, 36), Image.BOX), B.resize((64, 36), Image.BOX)
    d = ImageChops.difference(sa, sb)
    px = list(d.getdata())
    mean = sum(sum(p) / 3.0 for p in px) / len(px)
    # largest mean difference of an 8x8-pixel block of the 64x36 grid, by blocks of 8x6 cells
    mb = 0.0
    for by in range(0, 36, 6):
        for bx in range(0, 64, 8):
            vals = [sum(d.getpixel((x, y))) / 3.0 for y in range(by, min(by + 6, 36)) for x in range(bx, min(bx + 8, 64))]
            mb = max(mb, sum(vals) / len(vals))
    return {"mean": round(mean, 3), "max_block": round(mb, 3), "ok": mean <= tol["mean"] and mb <= tol["max_block"]}


def diff_route(rundir, golden_root, tid, route, spec, frames):
    gdir = os.path.join(golden_root, tid, route)
    tol = dict(MANIFEST["defaults"]); tol.update({k: spec[k] for k in ("mean", "max_block") if k in spec})
    out = {}
    for f in frames:
        shot = os.path.join(rundir, "frame-%d.png" % f)
        gold = os.path.join(gdir, "frame-%d.png" % f)
        if not os.path.exists(shot):
            out[str(f)] = {"status": "missing"}                # the run never reached that frame
        elif not os.path.exists(gold):
            out[str(f)] = {"status": "new"}
        else:
            r = compare(gold, shot, tol); r["status"] = "ok" if r["ok"] else "differs"; out[str(f)] = r
    return out


def run_route(a, tid, route, spec, stamp_dir):
    t = MANIFEST["titles"][tid]
    rd = os.path.join(stamp_dir, tid, route); os.makedirs(rd, exist_ok=True)
    dd = data_dir(a.data_root, t["title_id"])
    frames = spec["frames"]
    env = dict(os.environ)
    env.update({"NAPPLE_INPUT_SCRIPT": spec["input"], "NAPPLE_SCREENSHOT": os.path.join(rd, "frame"), "NAPPLE_SCREENSHOT_FRAMES": ",".join(map(str, frames)),
                "NAPPLE_PERF_LOG": "1"})
    if a.validation and MANIFEST["defaults"].get("validation", True) and spec.get("validation", True):
        env["MTL_DEBUG_LAYER"] = "1"
        if a.shader_validation:
            # Metal shader validation crashes while loading a binary archive (MTLMetalScriptBuilder initFromScript, seen on macOS 27 / M5): such runs build without the archive.
            # It also slows shaders enough to trip the GPU watchdog (LAS: 2 `kIOGPUCommandBufferCallbackErrorHang` in 110 s), which then counts as a validation failure.
            env.update({"MTL_SHADER_VALIDATION": "1", "NAPPLE_GFX_NO_ARCHIVE": "1"})
    save = os.path.join(rd, "save")
    cmd = [a.runtime, dd, "--headless", "--seconds", str(spec["seconds"]), "--save-root", save]
    t0 = time.time()
    with open(os.path.join(rd, "log.txt"), "w") as lf:
        rc = subprocess.call(cmd, env=env, stdout=lf, stderr=subprocess.STDOUT, cwd=ROOT)
    text = open(os.path.join(rd, "log.txt"), errors="replace").read()
    warn, val = scan_log(text)
    m = re.search(r"stats: frames (\d+)", text)
    res = {"title": tid, "route": route, "exit": rc, "seconds": round(time.time() - t0, 1), "frames_run": int(m.group(1)) if m else 0,
           "crash": "CRASH" in text, "gfx_warnings": {k: {"count": len(v), "first": v[0]} for k, v in warn.items()},
           "validation": {k: {"count": len(v), "first": v[0]} for k, v in val.items()}}
    res["frames"] = diff_route(rd, a.golden_root, tid, route, spec, frames)
    shutil.rmtree(save, ignore_errors=True)                  # scratch saves are large and not evidence
    bad = res["exit"] != 0 or res["crash"] or res["gfx_warnings"] or res["validation"] or any(v["status"] in ("differs", "missing") or (a.strict and v["status"] == "new") for v in res["frames"].values())
    res["pass"] = not bad
    json.dump(res, open(os.path.join(rd, "result.json"), "w"), indent=2)
    return res


def write_sheet(stamp_dir):
    rows = []
    for rj in sorted(glob.glob(os.path.join(stamp_dir, "*", "*", "result.json"))):
        r = json.load(open(rj)); base = os.path.relpath(os.path.dirname(rj), stamp_dir)
        cells = []
        for f, v in r["frames"].items():
            img = "%s/frame-%s.png" % (base, f)
            cells.append('<figure class="%s"><img loading="lazy" src="%s"><figcaption>frame %s: %s%s</figcaption></figure>' % (
                v["status"], html.escape(img), f, v["status"], (" (mean %.2f, block %.2f)" % (v["mean"], v["max_block"])) if "mean" in v else ""))
        warn = "".join("<li>%s x%d: <code>%s</code></li>" % (html.escape(k), w["count"], html.escape(w["first"])) for k, w in r["gfx_warnings"].items())
        val = "".join("<li>%s x%d: <code>%s</code></li>" % (html.escape(k), w["count"], html.escape(w["first"])) for k, w in r["validation"].items())
        rows.append('<section><h2>%s / %s <span class="%s">%s</span></h2><p>exit %s, %s frames in %ss%s</p>%s%s<div class="grid">%s</div></section>' % (
            r["title"], r["route"], "ok" if r["pass"] else "differs", "PASS" if r["pass"] else "FAIL", r["exit"], r["frames_run"], r["seconds"], ", CRASH" if r["crash"] else "",
            ("<p>gfx warnings:</p><ul>%s</ul>" % warn) if warn else "", ("<p>Metal validation:</p><ul>%s</ul>" % val) if val else "", "".join(cells)))
    css = ("body{font:14px -apple-system,sans-serif;margin:16px;background:#fff;color:#111}@media(prefers-color-scheme:dark){body{background:#111;color:#eee}}"
           ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(320px,1fr));gap:8px}figure{margin:0}img{width:100%;border:3px solid #888}"
           "figure.ok img{border-color:#2a2}figure.differs img,figure.missing img{border-color:#c22}figure.new img{border-color:#c80}.ok{color:#2a2}.differs{color:#c22}code{font-size:12px}")
    open(os.path.join(stamp_dir, "contact.html"), "w").write("<!doctype html><meta charset=utf-8><title>Golden frames</title><style>%s</style><h1>Golden frames</h1>"
        "<p>Orange = no golden yet (review, then <code>golden.py approve</code>); red = differs from the golden or never reached.</p>%s" % (css, "".join(rows)))
    return os.path.join(stamp_dir, "contact.html")


def cmd_run(a):
    stamp = os.path.join(a.out, datetime.datetime.now().strftime("%Y%m%d-%H%M%S")); os.makedirs(stamp)
    results = []
    for t in titles_for(a.title):
        for route, spec in MANIFEST["titles"][t]["routes"].items():
            if a.route and route != a.route:
                continue
            print("== %s / %s" % (t, route), flush=True)
            r = run_route(a, t, route, spec, stamp); results.append(r)
            print("   %s  exit %s frames %s warnings %d validation %d crash %s" % ("PASS" if r["pass"] else "FAIL", r["exit"], r["frames_run"], len(r["gfx_warnings"]), len(r["validation"]), r["crash"]))
    json.dump({"results": results}, open(os.path.join(stamp, "report.json"), "w"), indent=2)
    print("contact sheet:", write_sheet(stamp))
    return 0 if all(r["pass"] for r in results) else 1


def cmd_diff(a):
    for rj in sorted(glob.glob(os.path.join(a.rundir, "*", "*", "result.json"))):
        r = json.load(open(rj))
        spec = MANIFEST["titles"][r["title"]]["routes"][r["route"]]
        r["frames"] = diff_route(os.path.dirname(rj), a.golden_root, r["title"], r["route"], spec, spec["frames"])
        json.dump(r, open(rj, "w"), indent=2)
        print(r["title"], r["route"], {k: v["status"] for k, v in r["frames"].items()})
    print("contact sheet:", write_sheet(a.rundir))
    return 0


def cmd_approve(a):
    n = 0
    for rj in sorted(glob.glob(os.path.join(a.rundir, "*", "*", "result.json"))):
        r = json.load(open(rj))
        if (a.title and r["title"] != a.title) or (a.route and r["route"] != a.route):
            continue
        dst = os.path.join(a.golden_root, r["title"], r["route"]); os.makedirs(dst, exist_ok=True)
        for f in glob.glob(os.path.join(os.path.dirname(rj), "frame-*.png")):
            shutil.copy(f, dst); n += 1
    print("approved %d frames into %s" % (n, a.golden_root))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--golden-root", default=os.environ.get("NAPPLE_GOLDEN_ROOT", DEFAULT_GOLDEN))
    sp = ap.add_subparsers(dest="cmd", required=True)
    b = sp.add_parser("build"); b.add_argument("title"); b.add_argument("--data-root", default="/private/tmp/napple-golden-data"); b.add_argument("--napple"); b.add_argument("--testing", default=TESTING)
    r = sp.add_parser("run"); r.add_argument("title"); r.add_argument("--route"); r.add_argument("--data-root", default="/private/tmp/napple-golden-data")
    r.add_argument("--runtime", default=os.path.join(ROOT, "runtime/build/napple-runtime")); r.add_argument("--out", default="/private/tmp/napple-golden-runs")
    r.add_argument("--no-validation", dest="validation", action="store_false", help="no Metal API validation (MTL_DEBUG_LAYER)"); r.add_argument("--shader-validation", action="store_true", help="also MTL_SHADER_VALIDATION (slow; see the guide)"); r.add_argument("--strict", action="store_true", help="frames without a golden fail")
    for n in ("diff", "sheet", "approve"):
        p = sp.add_parser(n); p.add_argument("rundir")
        if n == "approve":
            p.add_argument("--title"); p.add_argument("--route")
    a = ap.parse_args()
    if a.cmd == "run":
        os.makedirs(a.out, exist_ok=True)
    return {"build": cmd_build, "run": cmd_run, "diff": cmd_diff, "approve": cmd_approve, "sheet": lambda x: print(write_sheet(x.rundir)) or 0}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main() or 0)
