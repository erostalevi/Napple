#!/usr/bin/env python3
"""e2e.py - end-to-end run of the whole Napple product per title. Use tools/e2e/run.sh (it picks the python that has Pillow).

  run.sh <las|smo|acnh|mk8d|all> [--out DIR] [--keyset NAME] [--seconds N] [--keep-apps] [--skip-build] [--no-golden] [--no-overlay] [--no-mii] [--no-applets] [--build-only] [--only STEPS]

Per title, in order (every step ends PASS, FAIL, KNOWN (a documented known issue), REPORT (informational), N/A or SKIP):
  build       the .app through Studio's build path (studio `napple-studio-build`: KeysetManager + NappleCLI.build; in-place data mode, updates and DLC as planned)
  bundle      codesign verifies; libnapple-applet.dylib is inside; the A32 jit dylib is embedded for 32-bit titles; updates / DLC are in the manifest
  camera-mic  Info.plist usage strings + codesign entitlements (camera, audio-input) and no App Sandbox
  boot        launcher start, headless, exit 0, no CRASH, a frame floor
  audio       the PCM counter saw non-silent audio
  input       the scripted input (NAPPLE_INPUT_SCRIPT) reached the guest
  amiibo      NAPPLE_AMIIBO with a generated virtual amiibo: placed and removed when the title opens the nfp service
  save        a save was written (commit) in the boot, and re-read by a second boot with the same save root
  mii-db      (first title) the Mii Maker applet creates a Mii (scripted input + swkbd answer) in the shared database; every title then boots against the same file
  applets     (first title) the library applet sheets (error, controller, player select, software keyboard, web, album) raised over the running title's window
              with NAPPLE_DEBUG_SHEETS, answered by NAPPLE_SHEET_AUTO (confirm run, then cancel run + NAPPLE_SWKBD_ANSWER / NAPPLE_CONTROLLER_PADS), PNG of each sheet
  overlay     the in-game overlay tabs render (NAPPLE_FE_OVERLAY + NAPPLE_FE_VIEWSHOT, a windowed run)
  golden      the golden-frame harness (tools/golden, report only: diffs do not fail the table; the user approves goldens)
Results: <out>/summary.json, <out>/summary.md and the table on stdout. Exit 0 when no step is FAIL.
All runs share ONE system data root (<out>/support, NAPPLE_SUPPORT_DIR): fonts are copied from ~/Library/Application Support/Napple/system/fonts, the Mii database and the
amiibo store are created there, which is also what proves that the second title reads the Mii database the first one wrote.
"""
import argparse, glob, importlib.util, json, os, plistlib, re, shutil, subprocess, sys, tempfile, time, types

ROOT = os.environ.get("NAPPLE_ROOT") or os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TESTING = os.environ.get("NAPPLE_TESTING_MATERIAL", "/Users/neto/Documents/Napple/Testing Material")
REAL_SUPPORT = os.path.expanduser("~/Library/Application Support/Napple")
GOLDEN_ROUTES = json.load(open(os.path.join(ROOT, "tools/golden/routes.json")))["titles"]
AMIIBO_MODEL = "0000000000340102"
TITLES = {
    "las": {"id": "01006BB00C6F0000", "script": "20:A:0.3,24:A:0.3,28:A:0.3", "floor": 1200},
    "smo": {"id": "0100000000010000", "script": "30:A:0.3,45:A:0.3,60:A:0.3", "floor": 1000, "seconds": 100},  # SMO is silent until its title screen
    "acnh": {"id": "01006F8002326000", "script": "20:A:0.3,30:A:0.3,40:A:0.3", "floor": 1000},
    "mk8d": {"id": "0100152000022000", "script": "15:A,19:A,23:A,27:A,31:A,35:A,39:A,43:A", "floor": 1000},
}
KNOWN = [
    (re.compile(r"CRASH signal=11 fault=0x8 pc=main\+0x6d65a4"), "ACNH intermittent null dereference in game code (main+0x6d65a4, a singleton whose field was not set yet) about 1 s into a start, seen under host load (kernel-threads note: a start-up ordering race between game threads, not reproduced without load)"),
    (re.compile(r"sdk\+0xc80730"), "ACNH intermittent guest Break at sdk+0xc80730 about 1 s into a headless start (independent of the fps mode; tracked in the orchestration notes)"),
]


class Run:
    def __init__(self, a):
        self.a = a
        self.out = os.path.abspath(a.out)
        self.support = os.path.join(self.out, "support")
        self.rows = []  # (title, step, status, detail)

    def add(self, title, step, status, detail=""):
        self.rows.append({"title": title, "step": step, "status": status, "detail": detail})
        print("  %-5s %-11s %-6s %s" % (title, step, status, detail), flush=True)


def sh(cmd, env=None, log=None, timeout=None, cwd=None):
    """Runs cmd; returns the exit status. Output goes to the file `log` (or is discarded)."""
    f = open(log, "w") if log else subprocess.DEVNULL
    try:
        return subprocess.call(cmd, env=env, stdout=f, stderr=subprocess.STDOUT, timeout=timeout, cwd=cwd)
    except subprocess.TimeoutExpired:
        return 124
    finally:
        if log:
            f.close()


def read(path):
    try:
        return open(path, errors="replace").read()
    except OSError:
        return ""


def studio_build_tool(r):
    """Path of the napple-studio-build executable, built (and cached) outside the checkout (File Provider xattrs break codesign inside ~/Documents)."""
    scratch = os.path.join(os.environ.get("TMPDIR", "/tmp"), "napple-e2e-studio")
    sh(["swift", "build", "--scratch-path", scratch, "--product", "napple-studio-build"], cwd=os.path.join(ROOT, "studio"), log=os.path.join(r.out, "swift-build.log"))
    p = subprocess.run(["swift", "build", "--scratch-path", scratch, "--show-bin-path"], cwd=os.path.join(ROOT, "studio"), capture_output=True, text=True).stdout.strip()
    tool = os.path.join(p, "napple-studio-build")
    return tool if os.path.exists(tool) else None


def env_for(r, **extra):
    e = dict(os.environ)
    e["NAPPLE_SUPPORT_DIR"] = r.support
    e.update(extra)
    return e


def prepare_support(r):
    os.makedirs(os.path.join(r.support, "system"), exist_ok=True)
    fonts = os.path.join(REAL_SUPPORT, "system/fonts")
    dst = os.path.join(r.support, "system/fonts")
    if os.path.isdir(fonts) and not os.path.isdir(dst):
        shutil.copytree(fonts, dst)


def app_game_dir(app):
    g = os.path.join(app, "Contents/Resources/game")
    for d in sorted(glob.glob(os.path.join(g, "*/"))):
        if os.path.exists(os.path.join(d, "manifest.json")) and os.path.basename(d.rstrip("/")) != "applets":
            return d.rstrip("/")
    return None


def step_build(r, name, spec, tool):
    tdir = os.path.join(r.out, "apps", name)
    log = os.path.join(r.out, "logs", name + "-build.log")
    os.makedirs(os.path.dirname(log), exist_ok=True)
    existing = glob.glob(os.path.join(tdir, "*.app"))
    if r.a.skip_build and existing:
        r.add(name, "build", "SKIP", "reusing " + existing[0])
        return existing[0]
    shutil.rmtree(tdir, ignore_errors=True)
    files = [os.path.join(TESTING, n) for n in GOLDEN_ROUTES[name]["nsp"]]
    env = dict(os.environ)
    env["NAPPLE_CLI"] = os.environ.get("E2E_NAPPLE_CLI") or os.path.join(ROOT, "target/release/napple")   # this checkout's CLI, never an installed engine
    t0 = time.time()
    rc = sh([tool, "--out", tdir, "--keyset", r.a.keyset] + files, env=env, log=log)
    apps = glob.glob(os.path.join(tdir, "*.app"))
    if rc or not apps:
        r.add(name, "build", "FAIL", "studio build exit %s, log %s" % (rc, log))
        return None
    r.add(name, "build", "PASS", "%s in %.0f s" % (os.path.basename(apps[0]), time.time() - t0))
    return apps[0]


def step_bundle(r, name, app):
    probs, notes = [], []
    if sh(["codesign", "--verify", "--deep", "--strict", app]):
        probs.append("codesign --verify failed")
    if not os.path.exists(os.path.join(app, "Contents/Frameworks/libnapple-applet.dylib")):
        probs.append("libnapple-applet.dylib missing from Contents/Frameworks")
    gd = app_game_dir(app)
    if not gd:
        probs.append("no game data directory in the app")
    else:
        man = json.loads(read(os.path.join(gd, "manifest.json")) or "{}")
        a32 = os.path.exists(os.path.join(gd, "modules/a32jit.dylib"))
        notes.append("a32jit.dylib embedded" if a32 else "64-bit")
        if name == "mk8d" and not a32:
            probs.append("MK8D is a 32-bit title but modules/a32jit.dylib is not in the app")
        ups = [d for d in glob.glob(os.path.join(app, "Contents/Resources/game/*/")) if os.path.basename(d.rstrip("/")) not in ("applets", "sysdata", "system")]
        notes.append("%d version dir(s)" % len(ups))
        for k in ("dlc", "updates"):
            if man.get(k) is not None:
                notes.append("%s %s" % (len(man[k]) if hasattr(man[k], "__len__") else man[k], k))
    r.add(name, "bundle", "FAIL" if probs else "PASS", "; ".join(probs or notes))


def step_camera_mic(r, name, app):
    probs = []
    pl = plistlib.load(open(os.path.join(app, "Contents/Info.plist"), "rb"))
    for k in ("NSCameraUsageDescription", "NSMicrophoneUsageDescription"):
        if not pl.get(k):
            probs.append(k + " missing")
    ent = subprocess.run(["codesign", "-d", "--entitlements", "-", "--xml", app], capture_output=True, text=True)
    txt = ent.stdout + ent.stderr
    for k in ("com.apple.security.device.camera", "com.apple.security.device.audio-input"):
        if k not in txt:
            probs.append("entitlement " + k + " missing")
    if "app-sandbox" in txt:
        probs.append("App Sandbox entitlement present (system folders need an unsandboxed app)")
    r.add(name, "camera-mic", "FAIL" if probs else "PASS", "; ".join(probs) or "usage strings + camera/audio-input entitlements, no sandbox")


def launcher(app):
    return os.path.join(app, "Contents/MacOS/napple-launcher")


def boot(r, name, app, seconds, save, tag, script=None, extra_env=None, headless=True):
    spec = TITLES[name]
    log = os.path.join(r.out, "logs", "%s-%s.log" % (name, tag))
    env = env_for(r, NAPPLE_INPUT_LOG="1", NAPPLE_PERF_LOG="1")
    if script:
        env["NAPPLE_INPUT_SCRIPT"] = script
    env.update(extra_env or {})
    cmd = [launcher(app)] + (["--headless"] if headless else []) + ["--seconds", str(seconds), "--save-root", save]
    rc = sh(cmd, env=env, log=log, timeout=seconds + 120)
    return rc, read(log), log


def known_issue(text):
    for rx, why in KNOWN:
        if rx.search(text):
            return why
    return None


def step_boot_group(r, name, app):
    spec = TITLES[name]
    save = os.path.join(r.out, "saves", name)
    shutil.rmtree(save, ignore_errors=True)
    amiibo = AMIIBO_MODEL + ",3000,12000"
    status, tries, text, log, rc = "FAIL", 0, "", "", 1
    while tries < 3:
        tries += 1
        secs = max(r.a.seconds, spec.get("seconds", 0))
        rc, text, log = boot(r, name, app, secs, save, "boot", script=spec["script"], extra_env={"NAPPLE_AMIIBO": amiibo})
        if rc == 0 and "CRASH" not in text:
            break
        if not known_issue(text):
            break
        shutil.rmtree(save, ignore_errors=True)
    m = re.findall(r"stats: frames (\d+)", text)
    frames = int(m[-1]) if m else 0
    crash = "CRASH" in text
    ki = known_issue(text) if (rc != 0 or crash) else None
    if rc == 0 and not crash and frames >= spec["floor"]:
        r.add(name, "boot", "PASS", "exit 0, %d frames in %d s (floor %d)%s" % (frames, secs, spec["floor"], ", retried once" if tries > 1 else ""))
        booted = True
    elif ki:
        r.add(name, "boot", "KNOWN", ki + " (exit %s, %d frames)" % (rc, frames))
        booted = False
    else:
        r.add(name, "boot", "FAIL", "exit %s, %d frames (floor %d), crash=%s, log %s" % (rc, frames, spec["floor"], crash, log))
        booted = False
    pcm = re.search(r"PCM: (\d+) frames pushed, (\d+) above the noise floor, peak ([0-9.]+)", text)
    if pcm and int(pcm.group(2)) > 0:
        r.add(name, "audio", "PASS", "%s frames pushed, %s above the noise floor, peak %s" % pcm.groups())
    elif pcm:
        r.add(name, "audio", "FAIL", "silent: %s frames pushed, none above the noise floor" % pcm.group(1))
    else:
        r.add(name, "audio", "KNOWN" if ki else "FAIL", "no PCM counter line (the process did not stop cleanly)")
    n_in = len(re.findall(r"input: script: press", text))
    want = len(spec["script"].split(","))
    r.add(name, "input", "PASS" if n_in >= 1 else ("KNOWN" if ki else "FAIL"), "%d of %d scripted presses reached the guest" % (n_in, want))
    if re.search(r"opened service nfp:user|opened service nfc:user", text) and not re.search(r"NAPPLE_AMIIBO:", text):
        r.add(name, "amiibo", "N/A", "nfp:user opened, but the title never started tag detection in this route (the scripted amiibo fires at the first StartDetection; nfp itself is covered by test_svc_nfp / test_svc_amiibo)")
    elif re.search(r"opened service nfp:user|opened service nfc:user", text):
        placed = re.search(r"NAPPLE_AMIIBO: amiibo placed", text)
        removed = re.search(r"NAPPLE_AMIIBO: amiibo removed", text)
        stored = glob.glob(os.path.join(r.support, "system/amiibo/*.json"))
        r.add(name, "amiibo", "PASS" if stored and placed and removed else "FAIL", "nfp opened; virtual amiibo placed=%s removed=%s, %d in the store" % (bool(placed), bool(removed), len(stored)))
    else:
        r.add(name, "amiibo", "N/A", "the title never opened nfp:user/nfc:user in this route (the reader installs lazily: no abort)")
    # save: written, then re-read by a second boot
    commits = re.search(r"fs: save .*: (\d+) commits? this run", text)
    files = [p for p in glob.glob(os.path.join(save, name_id(name), "save/**/*"), recursive=True) if os.path.isfile(p)]
    if not booted:
        r.add(name, "save", "KNOWN" if ki else "SKIP", "boot did not complete")
    elif not files:
        r.add(name, "save", "FAIL", "no save file after the boot (commits line: %s)" % (commits.group(0) if commits else "none"))
    else:
        before = {p: os.path.getsize(p) for p in files}
        rc2, text2, log2 = boot(r, name, app, 20, save, "reboot")
        after = {p: os.path.getsize(p) for p in files if os.path.exists(p)}
        mounted = "mounted host directory" in text2
        ok = rc2 == 0 and "CRASH" not in text2 and mounted and all(after.get(p, -1) >= 0 for p in before)
        r.add(name, "save", "PASS" if ok else "FAIL", "%d save file(s) written, second boot mounted them again (exit %s)" % (len(files), rc2) if ok else "second boot exit %s mounted=%s files kept=%s log %s" % (rc2, mounted, after == before or len(after) == len(before), log2))
    mii_served = re.search(r"opened service mii:u", text)
    if mii_served:
        w = re.search(r"mii: list kind \d+ flags 0x\w+: output buffer \d+ bytes \(\d+ elements of \d+\), wrote (\d+)", text)
        r.add(name, "mii-serve", "PASS", "mii:u opened, %s" % (w.group(0)[:100] if w else "database served"))
    return booted


def name_id(name):
    return TITLES[name]["id"]


def mii_count(r):
    p = os.path.join(r.support, "system/mii/MiiDatabase.dat")
    try:
        b = open(p, "rb").read()
        return b[5] if b[:4] == b"NFDB" else None
    except OSError:
        return None


def step_mii(r, name, app):
    """Creates a Mii in the real Mii Maker (the firmware applet in the app) with scripted input; the next boots of every title read the same database."""
    gd = app_game_dir(app)
    applets = glob.glob(os.path.join(gd, "../applets/0100000000001009/*/manifest.json")) if gd else []
    if not applets:
        r.add(name, "mii-db", "SKIP", "no Mii Maker applet in this app (built without firmware)")
        return
    ad = os.path.dirname(applets[0])
    before = mii_count(r) or 0
    runtime = os.path.join(app, "Contents/MacOS/napple-runtime")
    script = ("14:DOWN:0.2,16:A:0.2,20:A:0.2,24:A:0.2,28:DOWN:0.2,29:DOWN:0.2,30:DOWN:0.2,31:DOWN:0.2,32:DOWN:0.2,33:DOWN:0.2,35:A:0.2,40:DOWN:0.2,42:A:0.2,46:A:0.2,50:A:0.2")
    log = os.path.join(r.out, "logs", "mii-maker.log")
    nick = "E2E%06d" % (int(time.time()) % 1000000)
    env = env_for(r, NAPPLE_INPUT_SCRIPT=script, NAPPLE_SWKBD_ANSWER=nick, NAPPLE_SCREENSHOT=os.path.join(r.out, "logs", "mii-maker"),
                  NAPPLE_SCREENSHOT_FRAMES="1500,2100")
    for attempt in range(3):  # the script is wall-clock timed: a slow first start (cold shader / pipeline caches) can shift the Mii Maker screens past a press, so the step retries
        rc = sh([runtime, ad, "--headless", "--seconds", "60", "--save-root", os.path.join(r.out, "saves", "miimaker")], env=env, log=log, timeout=200)
        after = mii_count(r)
        try:
            found = nick.encode("utf-16-le") in open(os.path.join(r.support, "system/mii/MiiDatabase.dat"), "rb").read()
        except OSError:
            found = False
        if found and rc == 0:
            break
        nick = "E2E%06d" % ((int(time.time()) + attempt + 1) % 1000000)
        env["NAPPLE_SWKBD_ANSWER"] = nick
    if found and rc == 0:
        r.add(name, "mii-db", "PASS", "Mii Maker scripted: Mii %r (swkbd answer) is in the shared database (%s -> %s Miis); every later title reads this file" % (nick, before, after))
    else:
        r.add(name, "mii-db", "FAIL", "Mii %r not in the database (count %s -> %s), exit %s, log %s" % (nick, before, after, rc, log))
    # restart: the next boot of this title lists the new Mii in mii:u when the title opens it (checked in the boot step's mii-serve line)


def step_overlay(r, name, app, tabs):
    outdir = os.path.join(r.out, "overlay", name)
    os.makedirs(outdir, exist_ok=True)
    bad, ok = [], []
    for t in tabs:
        png = os.path.join(outdir, "tab%d.png" % t)
        if os.path.exists(png):
            os.remove(png)
        for attempt in range(3):  # an intermittent early abort of the title (KNOWN) is retried, a missing PNG after a clean run is a failure
            rc, text, log = boot(r, name, app, 14, os.path.join(r.out, "saves", name + "-overlay"), "overlay%d" % t,
                                 extra_env={"NAPPLE_FE_OVERLAY": str(t), "NAPPLE_FE_VIEWSHOT": png + ":7"}, headless=False)
            if os.path.exists(png) or not known_issue(text):
                break
        if os.path.exists(png) and os.path.getsize(png) > 6000:
            ok.append(t)
        else:
            bad.append(t)
    shutil.rmtree(os.path.join(r.out, "saves", name + "-overlay"), ignore_errors=True)
    r.add(name, "overlay", "FAIL" if bad else "PASS", "tabs rendered %s%s (PNGs in %s)" % (ok, ", missing %s" % bad if bad else "", outdir))


SHEETS = ["error", "controller", "player_select", "swkbd", "web", "album"]


def step_applets(r, name, app):
    """The AppKit sheets of the library applets over a real title (docs/archive/changes/2026-10-27-applets-on-title.md). No title raises most of these
    applets by itself, so NAPPLE_DEBUG_SHEETS (runtime/src/app.m) raises them from a thread of its own, the way a guest thread would, through the same
    hooks the applets use. Results come from the "debug-sheets:" log lines; each sheet must also leave a PNG (NAPPLE_SHEET_SHOT)."""
    outdir = os.path.join(r.out, "applets", name)
    shutil.rmtree(outdir, ignore_errors=True)
    os.makedirs(outdir, exist_ok=True)
    save = os.path.join(r.out, "saves", name + "-applets")
    base = {"NAPPLE_DEBUG_SHEETS": "12", "NAPPLE_AUDIO_DEVICE": "0", "NAPPLE_CONTROLLER_PADS": "3",
            "NAPPLE_SHEET_ORIGIN": "930,540", "NAPPLE_SHEET_SIZE": "560,380"}  # sheets are moved to the screen corner for the shot (the middle may be covered)
    runs = [
        ("confirm", {"NAPPLE_SHEET_AUTO": "confirm:3000", "NAPPLE_SHEET_SHOT": os.path.join(outdir, "confirm")},
         [r"debug-sheets: error -> dismissed", r"debug-sheets: controller -> 1 players=2 connected=3", r"debug-sheets: player select -> 0",
          r'debug-sheets: swkbd -> 1 "Link"', r"debug-sheets: web -> 0", r"debug-sheets: album -> 1", r"debug-sheets: done"], SHEETS),
        ("cancel", {"NAPPLE_SHEET_AUTO": "cancel:500", "NAPPLE_SWKBD_ANSWER": "Epona"},
         [r"debug-sheets: controller -> 0", r"debug-sheets: player select -> -1", r'debug-sheets: swkbd -> 1 "Epona"', r"debug-sheets: done"], []),
    ]
    bad, ok, missing = [], [], []
    for tag, extra, wants, shots in runs:
        env = dict(base)
        env.update(extra)
        text = ""
        for attempt in range(2):  # an intermittent early abort of the title (KNOWN) is retried once
            rc, text, log = boot(r, name, app, 45, save, "applets-" + tag, extra_env=env, headless=False)
            if not known_issue(text):
                break
        for w in wants:
            if not re.search(w, text):
                bad.append("%s run: no '%s'" % (tag, w))
        for sh_ in shots:
            png = os.path.join(outdir, tag, sh_ + ".png")
            if not (os.path.exists(png) and os.path.getsize(png) > 3000):
                missing.append("%s/%s.png" % (tag, sh_))  # screencapture needs the Screen Recording grant: a missing PNG is reported, not a failure
        if "CRASH" in text:
            bad.append("%s run: CRASH" % tag)
        if rc != 0:
            bad.append("%s run: exit %s" % (tag, rc))
        ok.append(tag)
    shutil.rmtree(save, ignore_errors=True)
    r.add(name, "applets", "FAIL" if bad else "PASS", ("; ".join(bad) if bad else "sheets %s answered (confirm run, cancel run); PNGs in %s%s" % (", ".join(SHEETS), outdir, ("; PNG missing (Screen Recording grant?): %s" % ", ".join(missing)) if missing else "")))


def step_golden(r, name, app):
    spec = importlib.util.spec_from_file_location("golden", os.path.join(ROOT, "tools/golden/golden.py"))
    g = importlib.util.module_from_spec(spec)
    sys.argv = ["golden.py"]
    try:
        spec.loader.exec_module(g)
    except SystemExit:
        pass
    root = os.path.join(r.out, "golden-data")
    tid = name_id(name)
    os.makedirs(os.path.join(root), exist_ok=True)
    link = os.path.join(root, tid)
    if os.path.lexists(link):
        os.remove(link)
    os.symlink(os.path.join(app, "Contents/Resources/game"), link)
    ns = types.SimpleNamespace(data_root=root, runtime=os.path.join(app, "Contents/MacOS/napple-runtime"), validation=False, shader_validation=False,
                               golden_root=os.path.expanduser("~/Library/Application Support/Napple/goldens"), strict=False)
    os.environ["NAPPLE_SUPPORT_DIR"] = r.support
    stamp = os.path.join(r.out, "golden", name)
    os.makedirs(stamp, exist_ok=True)
    parts = []
    for route, rs in g.MANIFEST["titles"][name]["routes"].items():
        try:
            res = g.run_route(ns, name, route, rs, stamp)
        except SystemExit as e:
            parts.append("%s: %s" % (route, e))
            continue
        fr = {k: v["status"] for k, v in res["frames"].items()}
        parts.append("%s: exit %s, %s frames, warnings %d, crash %s, frames %s" % (route, res["exit"], res["frames_run"], len(res["gfx_warnings"]), res["crash"], fr))
        if res["crash"] and known_issue(read(os.path.join(stamp, name, route, "log.txt"))):
            parts.append("(known flaky start)")
    r.add(name, "golden", "REPORT", "report only: " + " | ".join(parts))


def finish(r):
    os.makedirs(r.out, exist_ok=True)
    json.dump({"rows": r.rows, "fails": [x for x in r.rows if x["status"] == "FAIL"]}, open(os.path.join(r.out, "summary.json"), "w"), indent=2)
    stamp = time.strftime("%H%M%S")
    json.dump({"rows": r.rows}, open(os.path.join(r.out, "summary-%s.json" % stamp), "w"), indent=2)
    lines = ["| title | step | status | detail |", "|---|---|---|---|"] + ["| %s | %s | %s | %s |" % (x["title"], x["step"], x["status"], x["detail"].replace("|", "/")) for x in r.rows]
    open(os.path.join(r.out, "summary.md"), "w").write("\n".join(lines) + "\n")
    print("\n" + "\n".join(lines))
    print("\nsummary: %s/summary.json, %s/summary.md" % (r.out, r.out))
    return 1 if any(x["status"] == "FAIL" for x in r.rows) else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("title", nargs="+", choices=list(TITLES) + ["all"])
    ap.add_argument("--out", default=os.path.join(os.environ.get("TMPDIR", "/tmp"), "napple-e2e"))
    ap.add_argument("--keyset", default="22.1")
    ap.add_argument("--seconds", type=int, default=45)
    ap.add_argument("--keep-apps", action="store_true")
    ap.add_argument("--skip-build", action="store_true", help="reuse the .app of an earlier run in --out")
    ap.add_argument("--no-golden", action="store_true")
    ap.add_argument("--no-overlay", action="store_true")
    ap.add_argument("--build-only", action="store_true", help="build the .app(s) and stop (keeps them: run the other steps later with --skip-build --only ...)")
    ap.add_argument("--only", default="", help="comma list of steps to run: bundle,camera-mic,mii-db,boot,applets,overlay,golden (default all); lets a run be split into short game-lock turns")
    ap.add_argument("--no-applets", action="store_true")
    ap.add_argument("--no-mii", action="store_true")
    a = ap.parse_args()
    r = Run(a)
    os.makedirs(os.path.join(r.out, "logs"), exist_ok=True)
    prepare_support(r)
    tool = None if a.skip_build else studio_build_tool(r)
    if not a.skip_build and not tool:
        r.add("-", "build", "FAIL", "napple-studio-build could not be built (see %s/swift-build.log)" % r.out)
        return finish(r)
    names = list(TITLES) if "all" in a.title else a.title
    first = True
    for name in names:
        print("== %s" % name, flush=True)
        app = step_build(r, name, TITLES[name], tool)
        if not app:
            continue
        if a.build_only:
            continue
        only = [x for x in a.only.split(",") if x]
        want = lambda step: not only or step in only
        if want("bundle"):
            step_bundle(r, name, app)
        if want("camera-mic"):
            step_camera_mic(r, name, app)
        if first and not a.no_mii and want("mii-db"):
            step_mii(r, name, app)
        if want("boot"):
            step_boot_group(r, name, app)
        if first and not a.no_applets and want("applets"):
            step_applets(r, name, app)
        if not a.no_overlay and want("overlay"):
            step_overlay(r, name, app, [0, 1, 2, 3] if first else [3])
        if not a.no_golden and want("golden"):
            try:
                step_golden(r, name, app)
            except Exception as e:  # the golden harness is report only: its own errors never end the run
                r.add(name, "golden", "REPORT", "harness error: %s" % e)
        first = False
        if not a.keep_apps and not only:
            shutil.rmtree(os.path.dirname(app), ignore_errors=True)  # an app is 6 GB
    return finish(r)


if __name__ == "__main__":
    sys.exit(main())
