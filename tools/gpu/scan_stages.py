#!/usr/bin/env python3
"""scan_stages.py - which GPU stages does a Switch title use, without playing it.

  scan_stages.py scan <dir|romfs image>... [--title-id ID] [-o out.json] [--quick]
                                                           scan CTL_DUMP directories (*.ctl), extracted shader archives, or RomFS images
                                                           (`napple build --dump-romfs`, base / patched update / DLC): files inside SARC
                                                           archives and Yaz0 / zstd compression are opened (model archives carry shader
                                                           programs too: MK8D course and driver .szs); --quick inflates only paths that
                                                           look like shader data
  scan_stages.py cover scan1.json scan2.json ...            smallest set of titles covering every feature seen

Layout offsets are read from gfx/src/shader/control.h / control.c (see parse_offsets); nothing is duplicated silently:
if the header no longer defines an offset the tool falls back to the value below and says so on stderr.
Python 3 standard library only. Tested on synthetic sections; not verified on real titles (Mac check: docs/guides/gpu-title-scan.md).
"""
import argparse, hashlib, itertools, json, mmap, os, re, struct, sys

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
    r = {"stage": STAGE_NAMES[stage], "length": length, "code_len": code_len, "features": [],
         "digest": hashlib.sha1(bytes(sec[:length])).hexdigest()[:16]}  # the same program sits in many archives: distinct = distinct digests
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
    size, out, p, n_in = struct.unpack_from(">I", b, 4)[0], bytearray(), 16, len(b)
    while len(out) < size and p < n_in:
        code = b[p]; p += 1
        for bit in range(8):
            if len(out) >= size or p >= n_in:
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
                if dist >= n:  # no overlap: one slice copy
                    s = len(out) - dist
                    out += out[s:s + n]
                else:
                    for _ in range(n):
                        out.append(out[-dist])
    return bytes(out)


try:  # Python 3.14+: zstd in the standard library (ACNH's .zs files); older Pythons count them as not inflated
    from compression import zstd as _zstd
except ImportError:
    _zstd = None
ZSTD_MAGIC = b"\x28\xb5\x2f\xfd"
SHADERISH = re.compile(r"shader|bfsha|bnsh|sharc|nvn|gfx|effect|ptcl", re.I)


def inflate(data, stats):
    """Yaz0 / zstd payload -> bytes; None when it cannot be inflated here (counted in stats)."""
    try:
        if data[:4] == b"Yaz0":
            return yaz0(data)
        if data[:4] == ZSTD_MAGIC:
            if _zstd is None:
                stats["zstd_skipped"] = stats.get("zstd_skipped", 0) + 1
                return None
            return _zstd.decompress(data)
    except Exception:  # truncated / dictionary-compressed: counted, never fatal
        stats["inflate_failed"] = stats.get("inflate_failed", 0) + 1
        return None
    return data


def sarc_files(b):
    """Yield (name index, bytes) of the files of a SARC archive (little or big endian); nothing when b is not one."""
    if len(b) < 0x20 or b[:4] != b"SARC":
        return
    e = "<" if b[6:8] == b"\xff\xfe" else ">"
    data_off = struct.unpack_from(e + "I", b, 0xc)[0]
    if b[0x14:0x18] != b"SFAT":
        return
    n = struct.unpack_from(e + "H", b, 0x1a)[0]
    for i in range(n):
        o = 0x20 + 16 * i
        if o + 16 > len(b):
            return
        start, end = struct.unpack_from(e + "II", b, o + 8)
        if data_off + end <= len(b) and start <= end:
            yield i, b[data_off + start:data_off + end]


def scan_blob(data, name, progs, stats, inflate_all, depth=0):
    """Search one file's bytes (inflating and opening archives) for control sections; appends to progs."""
    if data[:4] in (b"Yaz0", ZSTD_MAGIC):
        if not (inflate_all or SHADERISH.search(name)):
            stats["compressed_not_opened"] = stats.get("compressed_not_opened", 0) + 1
            return
        data = inflate(data, stats)
        if data is None:
            return
        stats["inflated"] = stats.get("inflated", 0) + 1
    if data[:4] == b"SARC" and depth < 4:
        for i, f in sarc_files(data):
            scan_blob(f, "%s/#%d" % (name, i), progs, stats, inflate_all or bool(SHADERISH.search(name)), depth + 1)
        return
    got = list(find_sections(data)) if len(data) >= 0x760 else []
    if got:
        stats["files_with_sections"] = stats.get("files_with_sections", 0) + 1
    for d in got:
        d["file"] = name
        progs.append(d)


ROMFS_HDR = 0x50
ROMFS_MAX_FILE = 512 << 20  # larger files (movies, streamed audio) are not searched


def romfs_entries(m):
    """Yield (path, offset, size) of the files of a RomFS image (mmap / bytes). Raises ValueError when m is not one."""
    hdr = struct.unpack_from("<10Q", m, 0)
    if hdr[0] != ROMFS_HDR:
        raise ValueError("not a RomFS image")
    dmeta, dmeta_sz, fmeta, fmeta_sz, data = hdr[3], hdr[4], hdr[7], hdr[8], hdr[9]
    dirs = {}
    o = 0
    while o + 0x18 <= dmeta_sz:
        parent, _, _, _, _, nl = struct.unpack_from("<6I", m, dmeta + o)
        dirs[o] = (parent, bytes(m[dmeta + o + 0x18:dmeta + o + 0x18 + nl]).decode("utf-8", "replace"))
        o += (0x18 + nl + 3) & ~3

    def path_of(d):
        parts, seen = [], 0
        while d in dirs and d != 0 and seen < 64:
            parts.append(dirs[d][1]); d = dirs[d][0]; seen += 1
        return "/".join(reversed(parts))
    o = 0
    while o + 0x20 <= fmeta_sz:
        parent, _, off, size, _, nl = struct.unpack_from("<IIQQII", m, fmeta + o)
        name = bytes(m[fmeta + o + 0x20:fmeta + o + 0x20 + nl]).decode("utf-8", "replace")
        dp = path_of(parent)
        yield (dp + "/" if dp else "") + name, data + off, size
        o += (0x20 + nl + 3) & ~3


def is_romfs(path):
    try:
        with open(path, "rb") as fh:
            h = fh.read(16)
        return len(h) == 16 and struct.unpack_from("<Q", h)[0] == ROMFS_HDR
    except OSError:
        return False


def scan_romfs(path, inflate_all=True, label=None):
    """Scan every file of a RomFS image for control sections (archives and compression opened). Returns (progs, stats)."""
    progs, stats = [], {"files": 0, "skipped_large": 0}
    label = label or os.path.basename(path)
    with open(path, "rb") as fh, mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ) as m:
        for name, off, size in romfs_entries(m):
            stats["files"] += 1
            if size > ROMFS_MAX_FILE or off + size > len(m):
                stats["skipped_large"] += 1
                continue
            scan_blob(m[off:off + size], label + ":" + name, progs, stats, inflate_all)
    return progs, stats


def scan_dir(path, inflate_all=True, stats=None):
    """Scan a directory: *.ctl files are decoded directly, any other file is searched (archives / compression opened)."""
    progs, stats = [], stats if stats is not None else {}
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
            if f.endswith((".code", ".json")) or len(data) < 0x10:
                continue
            scan_blob(data, os.path.relpath(fp, path), progs, stats, inflate_all)
    return progs, stats.get("files_with_sections", 0)


def scan_paths(paths, inflate_all=True):
    """scan_dir / scan_romfs over several inputs (a title's base, update and DLC). Returns (progs, stats)."""
    progs, stats = [], {}
    for p in paths:
        if os.path.isdir(p):
            pp, _ = scan_dir(p, True, stats)
            for d in pp:
                d["file"] = os.path.basename(os.path.abspath(p)) + ":" + d["file"]
        elif is_romfs(p):
            pp, st = scan_romfs(p, inflate_all)
            for k, v in st.items():
                stats[k] = stats.get(k, 0) + v
        else:
            pp = []
            with open(p, "rb") as fh:
                scan_blob(fh.read(), os.path.basename(p), pp, stats, True)
        progs += pp
    return progs, stats


def summarize(progs, title_id, stats=None):
    st = {k: 0 for k in ("vs", "fs", "gs", "tcs", "tes", "cs", "xfb", "tess_isoline", "tess_triangle", "tess_quad")}
    seen, rare, distinct = set(), [], {}
    for p in progs:  # the same program is dumped or archived more than once: count distinct by content key
        key = (p.get("file"), p.get("offset"), p["stage"], p["code_len"], tuple(p["features"]))
        if key in seen:
            continue
        seen.add(key)
        distinct.setdefault(p["stage"], set()).add(p.get("digest"))
        st[p["stage"]] += 1
        for f in p["features"]:
            st[f] += 1
        if p["stage"] in ("gs", "tcs", "tes") or p["features"]:
            rare.append({k: p[k] for k in ("file", "offset", "stage", "length", "code_len", "features", "digest") if k in p})
    r = {"title_id": title_id, "programs": len(seen), "stages": st, "not_decodable": UNDECODABLE,
         "distinct": {k: len(v) for k, v in sorted(distinct.items())},
         "features": sorted(f for f in FEATURES if st[f]), "offsets_from": OFF_SOURCE, "stage_programs": rare[:200]}
    if stats is not None:
        r["inputs"] = stats
    return r


def human(s):
    st = s["stages"]
    lines = ["title %s: %d programs" % (s["title_id"], s["programs"]),
             "  stages: " + ", ".join("%s=%d" % (k, v) for k, v in st.items() if v),
             "  distinct control sections: " + ", ".join("%s=%d" % kv for kv in s.get("distinct", {}).items()),
             "  features: " + (", ".join(s["features"]) or "none beyond vs/fs"),
             "  not decodable yet (no known control-section field): " + ", ".join(s["not_decodable"])]
    if s.get("inputs"):
        lines.append("  inputs: " + ", ".join("%s=%d" % kv for kv in sorted(s["inputs"].items())))
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
    s = sub.add_parser("scan"); s.add_argument("dir", nargs="+"); s.add_argument("--title-id", default=None); s.add_argument("-o", "--out")
    s.add_argument("--quick", action="store_true", help="inflate only compressed RomFS files whose path looks like shader data (misses model archives)")
    c = sub.add_parser("cover"); c.add_argument("scans", nargs="+")
    a = ap.parse_args(argv)
    if a.cmd == "scan":
        progs, stats = scan_paths(a.dir, not a.quick)
        res = summarize(progs, a.title_id or os.path.basename(os.path.abspath(a.dir[0])), stats)
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
