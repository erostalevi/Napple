#!/usr/bin/env python3
"""render_scale.py - render scale / aspect screenshot matrix for the four Testing Material titles (docs/architecture/gfx.md, "Render scale").

  render_scale.py shots  <title|all> [--runtime R] [--out DIR] [--configs a,b,...]   run each configuration headless, capture frames, check sizes
  render_scale.py psnr   <dirA> <dirB>                                                 PSNR per frame between two shot directories (same title/config names)
  render_scale.py stress <title|all> [--seconds 60]                                    4x render scale no-crash run per title (exit 0, no gfx warning, no validation message)

A configuration is an output size (the headless drawable: NAPPLE_OUTPUT_SIZE, 16:9, 21:9, 9:16, ...) plus a settings.json (render scale, mode, fit). Scenes come from
tools/golden/routes.json (first route of each title, input script included). Output: <out>/<title>/<config>/frame-N.png + log.txt, summary.json.
The captured image is the drawable: its size must equal the configured output size (checked). Needs the game data built with `golden.py build` (see golden.py).
"""
import argparse, glob, json, math, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import golden  # noqa: E402

# scenes: a burst of 4 consecutive frames at each point (timing differs between runs, so comparisons between runs take the best matching pair)
CENTERS = {"las": [4500], "smo": [1500, 4000], "acnh": [300, 900], "mk8d": [1500, 3000]}
# animated scenes (LAS title screen, MK8D menu) drift between runs by seconds: a sweep of frames at a fixed step, compared pairwise
SWEEP = {"las": (4500, 15, 40), "mk8d": (1500, 15, 40), "acnh": (300, 15, 60)}
FRAMES = {t: [c + i for c in cs for i in range(4)] for t, cs in CENTERS.items()}
for t, (c0, step, n) in SWEEP.items():
    FRAMES[t] = [c0 + k * step for k in range(n)]
    CENTERS[t] = [c0]
# name: (output size, settings). aspect: 4 = free window keep aspect (bars), 3 stretch, 5 fill (crop), 7 whole-number multiple
CONFIGS = {
    "16x9-1x":      ((1280, 720),  {"render_mode": 0}),
    "16x9-2x":      ((1280, 720),  {"render_mode": 1, "internal_scale_percent": 200}),
    "16x9-4x":      ((1280, 720),  {"render_mode": 1, "internal_scale_percent": 400}),
    "16x9-0.5x":    ((1280, 720),  {"render_mode": 1, "internal_scale_percent": 50}),
    "16x9-1.37x":   ((1280, 720),  {"render_mode": 1, "internal_scale_percent": 137}),
    "4k-1x":        ((3840, 2160), {"render_mode": 0}),
    "4k-window":    ((3840, 2160), {"render_mode": 2}),
    "21x9-1x":      ((2560, 1080), {"render_mode": 0, "aspect": 4}),
    "21x9-window":  ((2560, 1080), {"render_mode": 2, "aspect": 4}),
    "21x9-fill":    ((2560, 1080), {"render_mode": 2, "aspect": 5}),
    "16x10-1x":     ((1920, 1200), {"render_mode": 0, "aspect": 4}),
    "4x3-1x":       ((1024, 768),  {"render_mode": 0, "aspect": 4}),
    "9x16-1x":      ((720, 1280),  {"render_mode": 0, "aspect": 4}),
    "9x16-window":  ((720, 1280),  {"render_mode": 2, "aspect": 4}),
    "16x9-fsr":     ((2560, 1440), {"render_mode": 2, "upscaler": 2, "aspect": 4}),
    "16x9-cas":     ((2560, 1440), {"render_mode": 2, "upscaler": 3, "aspect": 4}),
    "16x9-metalfx": ((2560, 1440), {"render_mode": 2, "upscaler": 1, "aspect": 4}),
    "21x9-report":  ((2560, 1080), {"render_mode": 2, "aspect": 6}),   # tell the game the window aspect (vi / applet), fit into the window
    "16x9-ssaa2":   ((1280, 720),  {"render_mode": 0, "supersample_percent": 200}),
}
DEFAULT_SET = ["16x9-1x", "16x9-2x", "16x9-4x", "21x9-window", "9x16-window"]


def run_one(a, tid, cfg_name, outdir, frames, seconds=None, extra_env=None):
    (w, h), st = CONFIGS[cfg_name]
    t = golden.MANIFEST["titles"][tid]
    route = list(t["routes"].values())[0]
    rd = os.path.join(outdir, tid, cfg_name); os.makedirs(rd, exist_ok=True)
    save = os.path.join(rd, "save"); sdir = os.path.join(save, t["title_id"]); os.makedirs(sdir, exist_ok=True)
    json.dump(dict({"version": 1}, **st), open(os.path.join(sdir, "settings.json"), "w"))
    env = dict(os.environ)
    env.update({"NAPPLE_INPUT_SCRIPT": route["input"], "NAPPLE_SCREENSHOT": os.path.join(rd, "frame"),
                "NAPPLE_SCREENSHOT_FRAMES": ",".join(map(str, frames)), "NAPPLE_OUTPUT_SIZE": "%dx%d" % (w, h), "NAPPLE_PERF_LOG": "1"})
    if a.validation:
        env["MTL_DEBUG_LAYER"] = "1"
    env.update(extra_env or {})
    pct = st.get("internal_scale_percent", 100) if st.get("render_mode") == 1 else 100
    fps = 45 if pct <= 150 else 28 if pct <= 250 else 14   # headless frame rate drops with the render size (LAS 4x: ~20 fps on an M5)
    secs = seconds or int(max(frames) / fps + 50)
    dd = golden.data_dir(a.data_root, t["title_id"])
    for attempt in range(3):
        with open(os.path.join(rd, "log.txt"), "w") as lf:
            rc = subprocess.call([a.runtime, dd, "--headless", "--seconds", str(secs), "--save-root", save], env=env, stdout=lf, stderr=subprocess.STDOUT, cwd=golden.ROOT)
        text = open(os.path.join(rd, "log.txt"), errors="replace").read()
        # LAS has a known start-up flake (guest Break in the first second, docs/archive/changes handle notes of 2026-10-26): a start-up failure is retried, anything later is not
        if rc != 0 and "FAIL: guest Break" in text and re.search(r"^\[napple\s+0\.\d+\] FAIL: guest Break", text, re.M):
            print("   (start-up flake, retry %d)" % (attempt + 1), flush=True)
            continue
        break
    warn, val = golden.scan_log(text)
    res = {"title": tid, "config": cfg_name, "exit": rc, "crash": "CRASH" in text, "warnings": {k: len(v) for k, v in warn.items()}, "validation": {k: len(v) for k, v in val.items()},
           "frames": {}}
    scaled = re.findall(r"render targets scaled x([\d.]+) / x([\d.]+)", text)
    res["scaled"] = scaled[-1] if scaled else None
    for f in frames:
        p = os.path.join(rd, "frame-%d.png" % f)
        if os.path.exists(p):
            from PIL import Image
            sz = Image.open(p).size
            res["frames"][f] = {"size": list(sz), "size_ok": sz == (w, h)}
        else:
            res["frames"][f] = {"missing": True}
    import shutil; shutil.rmtree(save, ignore_errors=True)
    return res


def cmd_shots(a):
    titles = golden.titles_for(a.title)
    cfgs = a.configs.split(",") if a.configs else DEFAULT_SET
    out = []
    for tid in titles:
        for c in cfgs:
            r = run_one(a, tid, c, a.out, FRAMES[tid])
            ok = r["exit"] == 0 and not r["crash"] and all(v.get("size_ok") for v in r["frames"].values())
            print("%-5s %-14s exit %s scaled %s %s %s" % (tid, c, r["exit"], r["scaled"], {k: v.get("size") or "MISSING" for k, v in r["frames"].items()}, "ok" if ok else "FAIL"), flush=True)
            out.append(r)
            json.dump(out, open(os.path.join(a.out, "summary.json"), "w"), indent=1)
    return 0


def psnr_files(pa, pb):
    import numpy as np
    from PIL import Image
    A = np.asarray(Image.open(pa).convert("RGB"), dtype=np.float64); B = np.asarray(Image.open(pb).convert("RGB"), dtype=np.float64)
    if A.shape != B.shape:
        return None
    mse = ((A - B) ** 2).mean()
    return 99.0 if mse == 0 else 10 * math.log10(255.0 ** 2 / mse)


def cmd_psnr(a):
    """Per title / config: the best PSNR over all pairs of frames of the same scene (bursts of CENTERS), so that run to run timing differences do not count."""
    bad = 0
    for da in sorted(glob.glob(os.path.join(a.a, "*", "*"))):
        rel = os.path.relpath(da, a.a); db = os.path.join(a.b, rel)
        if not os.path.isdir(db):
            continue
        tid = rel.split(os.sep)[0]
        for c in CENTERS.get(tid, []):
            best = None
            offs = [k * SWEEP[tid][1] for k in range(SWEEP[tid][2])] if tid in SWEEP else range(4)
            for i in offs:
                for j in offs:
                    pa, pb = os.path.join(da, "frame-%d.png" % (c + i)), os.path.join(db, "frame-%d.png" % (c + j))
                    if os.path.exists(pa) and os.path.exists(pb):
                        v = psnr_files(pa, pb)
                        if v is not None and (best is None or v > best):
                            best = v
            ok = best is not None and best > a.min
            bad += not ok
            print("%-26s scene@%-5d best PSNR %s dB %s" % (rel, c, "n/a" if best is None else "%.2f" % best, "ok" if ok else "BELOW %.0f" % a.min), flush=True)
    return 1 if bad else 0


def cmd_stress(a):
    bad = 0
    for tid in golden.titles_for(a.title):
        r = run_one(a, tid, a.config, a.out, [10 ** 9], seconds=a.seconds, extra_env={"NAPPLE_SCALE_DIAG": "1"})   # the diag lines feed the per-title audit
        ok = r["exit"] == 0 and not r["crash"] and not r["warnings"] and not r["validation"]
        print("%-5s 4x stress %ss: exit %s crash %s warnings %s validation %s scaled %s %s" % (tid, a.seconds, r["exit"], r["crash"], r["warnings"], r["validation"], r["scaled"], "ok" if ok else "FAIL"), flush=True)
        bad += not ok
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data-root", default="/private/tmp/napple-golden-data")
    sp = ap.add_subparsers(dest="cmd", required=True)
    for n in ("shots", "stress"):
        p = sp.add_parser(n); p.add_argument("title"); p.add_argument("--runtime", default=os.path.join(golden.ROOT, "runtime/build/napple-runtime")); p.add_argument("--out", default="/private/tmp/napple-rs-runs")
        p.add_argument("--no-validation", dest="validation", action="store_false")
        if n == "shots":
            p.add_argument("--configs", help="comma list of: " + ",".join(CONFIGS))
        else:
            p.add_argument("--seconds", type=int, default=60); p.add_argument("--config", default="16x9-4x")
    p = sp.add_parser("psnr"); p.add_argument("a"); p.add_argument("b"); p.add_argument("--min", type=float, default=45.0)
    a = ap.parse_args()
    return {"shots": cmd_shots, "psnr": cmd_psnr, "stress": cmd_stress}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main() or 0)
