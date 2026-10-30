#!/usr/bin/env python3
"""scan_stages.py - which GPU stages does a Switch title use, without playing it.

  scan_stages.py scan <dir> [--title-id ID] [-o out.json]   scan a CTL_DUMP directory (*.ctl) or a RomFS dump (shader archives)
  scan_stages.py cover scan1.json scan2.json ...            smallest set of titles covering every feature seen

Layout offsets are read from gfx/src/shader/control.h / control.c (see parse_offsets); nothing is duplicated silently:
if the header no longer defines an offset the tool falls back to the value below and says so on stderr.
Python 3 standard library only. Tested on synthetic sections; not verified on real titles (Mac check: docs/guides/gpu-title-scan.md).
"""
import argparse, itertools, json, os, re, struct, sys

MAGIC = 0x98761234
STAGE_NAMES = {0: "vs", 1: "fs", 2: "gs", 3: "tcs", 4: "tes", 5: "cs"}
# fallbacks, checked against control.h/.c by parse_offsets()
DEFAULTS = {"GLSLC_CONTROL_MIN": 0x760, "GLSLC_CONTROL_MAX": 0x1000, "GLSLC_XFB_BLOCK_OFF": 0x30, "GLSLC_TESS_MODE_OFF": 0x718,
            "len": 0x20, "const_size": 0x6fc, "const_off": 0x700, "code_len": 0x704, "stage": 0x714, "shared_mem": 0x754}
# features that cannot be decoded from a control section today (no known field): reported so the cover knows they are open
UNDECODABLE = ["msaa", "sparse"]
FEATURES = ["gs", "tcs", "tes", "xfb", "cs", "tess_isoline", "tess_triangle", "tess_quad"]


def parse_offsets(root=None):
    """Read the offsets from control.h/control.c when the source tree is reachable; returns (offsets, source)."""
    off = dict(DEFAULTS)
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(root or os.path.join(here, "..", ".."), "gfx", "src", "shader", "control.h")
    try:
        txt = open(path).read()
    except OSError:
        return off, "built-in defaults (control.h not found)"
    for m in re.finditer(r"#define\s+(GLSLC_\w+)\s+\(?(0x[0-9a-fA-F]+)u?\)?", txt):
        if m.group(1) in off:
            off[m.group(1)] = int(m.group(2), 16)
    csrc = os.path.join(os.path.dirname(path), "control.c")
    try:
        c = open(csrc).read()
        for key, pat in (("len", r"rd32\(ctl, (0x20)\)"), ("const_size", r"const_size = rd32\(ctl, (0x\w+)\)"),
                         ("const_off", r"const_off = rd32\(ctl, (0x\w+)\)"), ("code_len", r"code_len = rd32\(ctl, (0x\w+)\)"),
                         ("stage", r"stage = rd32\(ctl, (0x\w+)\)"), ("shared_mem", r"shared_mem = rd32\(ctl, (0x\w+)\)")):
            m = re.search(pat, c)
            if m:
                off[key] = int(m.group(1), 16)
            else:
                sys.stderr.write("scan_stages: control.c no longer matches %s, using fallback\n" % key)
    except OSError:
        pass
    return off, path


OFF, OFF_SOURCE = parse_offsets()


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def decode(sec, off=None):
    """Decode one control section (bytes). Returns a dict or None when it is not plausible (mirrors gfx_control_parse)."""
    off = off or OFF
    if len(sec) < off["GLSLC_CONTROL_MIN"] or u32(sec, 0) != MAGIC:
        return None
    n = u32(sec, off["len"])
    length = n if off["GLSLC_CONTROL_MIN"] <= n <= off["GLSLC_CONTROL_MAX"] else 0x880  # odd lengths are real (SMO/MK8D 0x7d1), as gfx_control_length
    stage, code_len = u32(sec, off["stage"]), u32(sec, off["code_len"])
    c_off, c_size = u32(sec, off["const_off"]), u32(sec, off["const_size"])
    if stage > 5 or not code_len or code_len > 16 << 20 or c_off > code_len or c_size > 0x10000 or len(sec) < length:
        return None
    r = {"stage": STAGE_NAMES[stage], "length": length, "code_len": code_len, "features": []}
    if stage == 0 or stage == 2 or stage == 4:
        if xfb(sec, length, off):
            r["features"].append("xfb")
    if stage == 4 and length > off["GLSLC_TESS_MODE_OFF"]:
        m = u32(sec, off["GLSLC_TESS_MODE_OFF"])
        if m and not m & ~0x333 and (m & 3) != 3 and ((m >> 4) & 3) != 3:
            r["features"].append(("tess_isoline", "tess_triangle", "tess_quad")[m & 3])
    if stage == 5:
        r["local"] = [u32(sec, off["shared_mem"] - 12 + 4 * i) for i in range(3)]
        r["shared_mem"] = u32(sec, off["shared_mem"])
    return r


def xfb(sec, length, off):
    """Same acceptance rule as gfx_control_xfb (a hypothesis: see control.h)."""
    base = off["GLSLC_XFB_BLOCK_OFF"]
    if length < base + 0x20 + 4 * 128:
        return False
    any_ = False
    for k in range(4):
        s, n = u32(sec, base + 4 * k), u32(sec, base + 0x10 + 4 * k)
        if s > 2048 or s & 3 or n > 128 or (n and not s):
            return False
        locs = sec[base + 0x20 + 128 * k: base + 0x20 + 128 * (k + 1)]
        if any(c != 0xff and c >= 0xc0 for c in locs[:n]):
            return False
        if any(c and c != 0xff for c in locs[n:]):
            return False
        if n and n * 4 > s:
            return False
        any_ |= n > 0
    return any_


def find_sections(blob, off=None):
    """Yield decoded sections found at 4-aligned offsets of an arbitrary blob (shader archives, dumps)."""
    magic = struct.pack("<I", MAGIC)
    i = blob.find(magic)
    while i >= 0:
        if i % 4 == 0:
            d = decode(blob[i:i + OFF["GLSLC_CONTROL_MAX"]], off)
            if d:
                d["offset"] = i
                yield d
        i = blob.find(magic, i + 1)


def yaz0(b):
    if b[:4] != b"Yaz0":
        return b
    size, out, p = struct.unpack_from(">I", b, 4)[0], bytearray(), 16
    while len(out) < size and p < len(b):
        code = b[p]; p += 1
        for bit in range(8):
            if len(out) >= size or p >= len(b):
                break
            if code & (0x80 >> bit):
                out.append(b[p]); p += 1
            else:
                b1, b2 = b[p], b[p + 1]; p += 2
                dist = ((b1 & 15) << 8 | b2) + 1
                n = b1 >> 4
                if n == 0:
                    n = b[p] + 0x12; p += 1
                else:
                    n += 2
                for _ in range(n):
                    out.append(out[-dist])
    return bytes(out)


def scan_dir(path):
    progs, files = [], 0
    for dp, _, fs in os.walk(path):
        for f in sorted(fs):
            fp = os.path.join(dp, f)
            try:
                with open(fp, "rb") as fh:
                    data = fh.read()
            except OSError:
                continue
            if f.endswith(".ctl"):
                d = decode(data)
                if d:
                    d["file"] = os.path.relpath(fp, path)
                    progs.append(d)
                continue
            if f.endswith((".code", ".json")) or len(data) < 0x760:
                continue
            data = yaz0(data) if data[:4] == b"Yaz0" else data
            got = list(find_sections(data))
            if got:
                files += 1
            for d in got:
                d["file"] = os.path.relpath(fp, path)
                progs.append(d)
    return progs, files


def summarize(progs, title_id):
    st = {k: 0 for k in ("vs", "fs", "gs", "tcs", "tes", "cs", "xfb", "tess_isoline", "tess_triangle", "tess_quad")}
    seen = set()
    for p in progs:  # the same program is dumped or archived more than once: count distinct by content key
        key = (p.get("file"), p.get("offset"), p["stage"], p["code_len"], tuple(p["features"]))
        if key in seen:
            continue
        seen.add(key)
        st[p["stage"]] += 1
        for f in p["features"]:
            st[f] += 1
    return {"title_id": title_id, "programs": len(seen), "stages": st, "not_decodable": UNDECODABLE,
            "features": sorted(f for f in FEATURES if st[f]), "offsets_from": OFF_SOURCE}


def human(s):
    st = s["stages"]
    lines = ["title %s: %d programs" % (s["title_id"], s["programs"]),
             "  stages: " + ", ".join("%s=%d" % (k, v) for k, v in st.items() if v),
             "  features: " + (", ".join(s["features"]) or "none beyond vs/fs"),
             "  not decodable yet (no known control-section field): " + ", ".join(s["not_decodable"])]
    return "\n".join(lines)


def cover(scans):
    """Greedy and (for <= 20 titles) exact minimal cover. scans: list of summaries. Returns dict."""
    sets = {s["title_id"]: frozenset(s["features"]) for s in scans}
    universe = frozenset().union(*sets.values()) if sets else frozenset()
    uncovered_ever = sorted(set(FEATURES) - universe)
    left, greedy = set(universe), []
    while left:
        t = max(sorted(sets), key=lambda k: len(sets[k] & left))
        if not sets[t] & left:
            break
        greedy.append(t); left -= sets[t]
    exact = None
    names = sorted(sets)
    if len(names) <= 20:
        for r in range(0, len(names) + 1):
            for c in itertools.combinations(names, r):
                if (frozenset().union(*(sets[k] for k in c)) if c else frozenset()) == universe:
                    exact = list(c); break
            if exact is not None:
                break
    return {"greedy": greedy, "exact": exact, "features_covered": sorted(universe), "features_no_title_covers": uncovered_ever,
            "not_decodable": UNDECODABLE}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("scan"); s.add_argument("dir"); s.add_argument("--title-id", default=None); s.add_argument("-o", "--out")
    c = sub.add_parser("cover"); c.add_argument("scans", nargs="+")
    a = ap.parse_args(argv)
    if a.cmd == "scan":
        progs, _ = scan_dir(a.dir)
        res = summarize(progs, a.title_id or os.path.basename(os.path.abspath(a.dir)))
        txt = json.dumps(res, indent=2)
        if a.out:
            open(a.out, "w").write(txt + "\n")
        else:
            print(txt)
        sys.stderr.write(human(res) + "\n")
        return 0
    res = cover([json.load(open(p)) for p in a.scans])
    print(json.dumps(res, indent=2))
    sys.stderr.write("smallest set: %s\ngreedy: %s\nno title covers: %s\n" % (res["exact"], res["greedy"], ", ".join(res["features_no_title_covers"]) or "-"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
