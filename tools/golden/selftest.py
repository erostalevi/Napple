#!/usr/bin/env python3
"""Self-test of the golden harness without a game: synthetic frames, a fake run directory, diff, contact sheet, approve. Run: python3 tools/golden/selftest.py"""
import importlib.util, json, os, sys, tempfile
from PIL import Image, ImageDraw

spec = importlib.util.spec_from_file_location("golden", os.path.join(os.path.dirname(os.path.abspath(__file__)), "golden.py"))
g = importlib.util.module_from_spec(spec); spec.loader.exec_module(g)

def img(path, color, box=None):
    im = Image.new("RGB", (320, 180), color)
    if box: ImageDraw.Draw(im).rectangle(box, fill=(255, 0, 255))
    im.save(path)

fail = 0
def check(c, msg):
    global fail
    if not c: print("FAIL", msg); fail += 1

with tempfile.TemporaryDirectory() as d:
    tol = {"mean": 3.0, "max_block": 40.0}
    a, b, c = (os.path.join(d, n) for n in ("a.png", "b.png", "c.png"))
    img(a, (40, 80, 120)); img(b, (41, 80, 121)); img(c, (40, 80, 120), (0, 0, 160, 90))
    check(g.compare(a, b, tol)["ok"], "near-identical frames pass")
    r = g.compare(a, c, tol); check(not r["ok"] and r["max_block"] > 40, "a magenta quarter of the frame fails (%s)" % r)
    w, v = g.scan_log("[napple-gfx] warn: CallCommands: stale or invalid command handle 0x1 ignored\n2026 Metal API Validation Enabled\nfailed assertion `x'\nfine\n[napple-gfx] stats: frames 1 unimplemented calls 0\n[napple-gfx] info: shader summary: 0 failed\n")
    check("stale or invalid command handle" in w and "failed assertion" in v and len(v) == 1, "log scan: %s %s" % (w, v))
    run = os.path.join(d, "run", "t", "r"); os.makedirs(run)
    img(os.path.join(run, "frame-1.png"), (40, 80, 120)); img(os.path.join(run, "frame-2.png"), (40, 80, 120), (0, 0, 160, 90))
    gold = os.path.join(d, "gold", "t", "r"); os.makedirs(gold)
    img(os.path.join(gold, "frame-1.png"), (40, 80, 121))
    res = g.diff_route(run, os.path.join(d, "gold"), "t", "r", {}, [1, 2, 3])
    check(res["1"]["status"] == "ok" and res["2"]["status"] == "new" and res["3"]["status"] == "missing", "diff statuses %s" % res)
    json.dump({"title": "t", "route": "r", "exit": 0, "seconds": 1, "frames_run": 10, "crash": False, "gfx_warnings": {}, "validation": {}, "pass": True, "frames": res}, open(os.path.join(run, "result.json"), "w"))
    sheet = g.write_sheet(os.path.join(d, "run")); check(os.path.exists(sheet) and "frame-1.png" in open(sheet).read(), "contact sheet written")
print("golden selftest: %s" % ("ok" if not fail else "%d FAILED" % fail)); sys.exit(1 if fail else 0)
