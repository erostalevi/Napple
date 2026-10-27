#!/usr/bin/env python3
"""frame_trace.py - headless frame-trace gate for the fps modes (docs/architecture/runtime-services.md, Timing).

  frame_trace.py [--titles las,smo,acne,mk8d] [--modes game,60,120,30,uncapped] [--seconds 20] [--data-root DIR]

Runs each title headless with NAPPLE_FRAME_TRACE (kind,real_ns,game_clock_ns for every guest vsync V and presented frame P) at each fps mode and checks:
  * game clock rate (game_clock / wall) = 1.00 +-0.02 over the run,
  * guest vsync rate ~ 60 Hz (+-3 %) whatever the mode: the host present rate must not reach the guest,
  * presents per second never above the vsync rate +3 % (frames are repeated by the display, never invented by the runtime).
Modes: game=0, display=1, <N>=custom N Hz, uncapped=3. Exit status 1 on a failed check or a non-zero game exit."""
import argparse, os, subprocess, sys, glob, json, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TITLES = {"las": "01006BB00C6F0000", "smo": "0100000000010000", "acne": "01006F8002326000", "mk8d": "0100152000022000"}


def data_dir(root, tid):
    base = os.path.join(root, tid)
    c = [d for d in glob.glob(os.path.join(base, "*")) if os.path.exists(os.path.join(d, "manifest.json"))]
    if not c:
        sys.exit("no data under %s (golden.py build)" % base)
    return max(c, key=os.path.getmtime)


def analyse(path):
    v, p = [], []
    for line in open(path).read().splitlines()[1:]:
        k, r, g = line.split(",")
        (v if k == "V" else p).append((int(r), int(g)))
    out = {"vsyncs": len(v), "presents": len(p)}
    if len(v) > 60:
        v = v[30:]                                   # skip start-up
        wall = (v[-1][0] - v[0][0]) / 1e9
        out["clock_rate"] = (v[-1][1] - v[0][1]) / (v[-1][0] - v[0][0])
        out["vsync_hz"] = (len(v) - 1) / wall
        if len(p) > 2:
            pw = [x for x in p if x[0] >= v[0][0]]
            out["present_hz"] = (len(pw) - 1) / ((pw[-1][0] - pw[0][0]) / 1e9) if len(pw) > 2 else 0
    if len(p) > 120:
        iv = sorted(p[i + 1][0] - p[i][0] for i in range(30, len(p) - 1))
        out["p99_over_median"] = iv[int(len(iv) * 0.99)] / iv[len(iv) // 2]
    if len(v) > 2:
        out["vsync_drift_frames"] = (len(v) - 1) - (v[-1][1] - v[0][1]) / 16666667.0   # vsync ticks against the game clock: <1 frame
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--titles", default="las,smo,acne,mk8d")
    ap.add_argument("--modes", default="game,60,120,30,uncapped")
    ap.add_argument("--seconds", type=int, default=20)
    ap.add_argument("--windowed", action="store_true", help="open the real window (soak): also checks p99 present interval < 1.5x median and vsync drift < 1 frame")
    ap.add_argument("--vsync", default="on", help="on|off (windowed: settings.json vsync for the run)")
    ap.add_argument("--data-root", default="/private/tmp/napple-golden-data")
    ap.add_argument("--runtime", default=os.path.join(ROOT, "runtime/build/napple-runtime"))
    a = ap.parse_args()
    bad = 0
    for t in a.titles.split(","):
        dd = data_dir(a.data_root, TITLES[t])
        for m in a.modes.split(","):
            env = dict(os.environ)
            env["NAPPLE_FPS_MODE"], env["NAPPLE_FPS_HZ"] = ("0", "") if m == "game" else ("1", "") if m == "display" else ("3", "") if m == "uncapped" else ("2", m)
            env["NAPPLE_VSYNC"] = "0" if a.vsync == "off" else "1"
            tmp = tempfile.mkdtemp(prefix="fpstrace")
            env["NAPPLE_FRAME_TRACE"] = os.path.join(tmp, "trace.csv")
            rc = subprocess.call([a.runtime, dd] + ([] if a.windowed else ["--headless"]) + ["--seconds", str(a.seconds), "--save-root", os.path.join(tmp, "save")], env=env,
                                 stdout=subprocess.DEVNULL, stderr=open(os.path.join(tmp, "log.txt"), "w"), cwd=ROOT)
            try:
                r = analyse(env["NAPPLE_FRAME_TRACE"])
            except Exception as e:
                r = {"error": str(e)}
            soak = not a.windowed or (r.get("p99_over_median", 99) < 1.5 and abs(r.get("vsync_drift_frames", 99)) < 1)
            ok = soak and rc == 0 and abs(r.get("clock_rate", 0) - 1.0) <= 0.02 and abs(r.get("vsync_hz", 0) - 60) <= 1.8 and r.get("present_hz", 0) <= r.get("vsync_hz", 0) * 1.03 + 0.5
            bad += not ok
            print("%-5s %-8s exit %d %s %s" % (t, m, rc, json.dumps(r), "PASS" if ok else "FAIL (%s)" % tmp), flush=True)
    sys.exit(1 if bad else 0)


main()
