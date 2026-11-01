#!/usr/bin/env python3
"""scan_stages.py - which GPU stages does a Switch title use, without playing it.

  scan_stages.py scan <dir|romfs image>... [--title-id ID] [-o out.json] [--quick]
                                                           scan CTL_DUMP directories (*.ctl), extracted shader archives, or RomFS images
                                                           (`napple build --dump-romfs`, base / patched update / DLC): files inside SARC
                                                           archives and Yaz0 / zstd compression are opened (model archives carry shader
                                                           programs too: MK8D course and driver .szs); --quick inflates only paths that
                                                           look like shader data
  scan_stages.py cover scan1.json scan2.json ...            smallest set of titles covering every feature seen
  scan_stages.py extract <dir|romfs image>... -o <corpus dir> --tag <title>
                                                           write every distinct program as <tag>_<digest>_<vs|ps|gs|tcs|tes|cs>.bin
                                                           (code area) + .ctl (control section) for the t_xlate corpus mode
                                                           (NAPPLE_SHADER_CORPUS). Code is paired with its control section through
                                                           the BNSH program record (u64 control offset, u64 code offset, relative
                                                           to the BNSH header; length = the section's +0x704 code length), or the
                                                           .code file next to a NAPPLE_GFX_CTL_DUMP .ctl; agl BAHS archives have no
                                                           readable record: each code area is written alone (stage from its SPH, no
                                                           .ctl). Game data: keep the corpus outside the repository.

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


CODE_MAGIC = struct.pack("<I", 0x12345678)  # first word of every GLSLC code area (then zeros up to the SPH at +0x30)
BNSH_MAGIC = b"BNSH"
CORPUS_STAGE = {"vs": "vs", "fs": "ps", "gs": "gs", "tcs": "tcs", "tes": "tes", "cs": "cs"}


def pair_code(data, got, stats):
    """Attach d["code"] (bytes) to the decoded sections `got` of blob `data` through the BNSH program records: per BNSH region, the 8-aligned u64
    values are indexed once and each section's offset relative to the BNSH header is looked up; the record starts with u64 control offset, u64 code
    offset (the size words after them differ between BNSH versions: bfsha u32 code size + u32 control size, VFXB effects others), the code must
    start with CODE_MAGIC (compute code has no header: instructions from its first word) and is the section's code_len (+0x704) bytes long. Sections without a record keep no code."""
    pair_bnsh(data, got, stats)
    if data[:4] == b"BAHS" and any("code" not in d for d in got):
        areas = code_areas(data, max(d["offset"] for d in got))
        stats["bahs_code_areas"] = stats.get("bahs_code_areas", 0) + len(areas)
        for d in got:
            if "code" not in d and d["stage"] != "cs":
                d["by_area"] = True  # its code is among the areas written below
        got_areas = areas
    else:
        got_areas = []
    for d in got_areas:
        d["area"] = True
        got.append(d)
    for d in got:
        if d.get("area"):
            continue
        if "code" in d:
            stats["paired"] = stats.get("paired", 0) + 1
        elif d.get("by_area"):
            stats["covered_by_code_areas"] = stats.get("covered_by_code_areas", 0) + 1
        else:
            stats["unpaired"] = stats.get("unpaired", 0) + 1


SPH_STAGE = {1: "vs", 2: "tcs", 3: "tes", 4: "gs", 5: "fs"}  # SPH word 0 bits 10..13 (ShaderType)


def code_areas(data, after):
    """agl BAHS archives (Odyssey's ShaderData / AglResource) store code areas after the control sections with no record this tool can read (they are
    shared between programs, not in section order): every CODE_MAGIC area on a 0x100 boundary after `after`, up to the next one, as a program of its own
    (stage from its SPH; no control section: the corpus mode then translates the whole area, constants included). Compute code has no header: not found."""
    out, cur = [], data.find(CODE_MAGIC, after)
    while cur >= 0:
        nxt = data.find(CODE_MAGIC, cur + 4)
        while nxt >= 0 and nxt % 0x100:
            nxt = data.find(CODE_MAGIC, nxt + 4)
        end = nxt if nxt >= 0 else len(data)
        if cur % 0x100 == 0 and cur + 0x80 <= end:
            st = SPH_STAGE.get((u32(data, cur + 0x30) >> 10) & 0xf)
            if st:
                out.append({"stage": st, "code": bytes(data[cur:end]), "ctl": b"", "offset": cur})
        cur = nxt
    return out


def pair_bnsh(data, got, stats):
    bnsh = []
    i = data.find(BNSH_MAGIC)
    while i >= 0:
        if i % 8 == 0:
            bnsh.append(i)
        i = data.find(BNSH_MAGIC, i + 1)
    if not bnsh:
        return
    bounds = bnsh[1:] + [len(data)]
    for bo, end in zip(bnsh, bounds):
        mine = [d for d in got if bo <= d["offset"] < end and "code" not in d]
        if not mine:
            continue
        want = {d["offset"] - bo: d for d in mine}
        n = (end - bo) // 8
        mv = memoryview(data)[bo:bo + 8 * n].cast("Q")
        for k, v in enumerate(mv):
            d = want.get(v)
            if d is None or "code" in d:
                continue
            p = bo + 8 * k
            if p + 24 > len(data):
                continue
            code_off = struct.unpack_from("<Q", data, p + 8)[0]
            c, code_size = bo + code_off, d["code_len"]
            if 0x80 <= code_size and c + code_size <= len(data) and (data[c:c + 4] == CODE_MAGIC or d["stage"] == "cs"):  # compute: no header
                d["code"] = bytes(data[c:c + code_size])
                d["ctl"] = bytes(data[d["offset"]:d["offset"] + d["length"]])
        mv.release()


class Corpus:
    """Writer of distinct programs for the t_xlate corpus mode: <dir>/<tag>_<digest of control + code>_<stage>.bin / .ctl."""

    def __init__(self, out, tag):
        self.out, self.tag, self.seen, self.written = out, tag, set(), {}
        os.makedirs(out, exist_ok=True)

    def add(self, d):
        if "code" not in d:
            return
        c_off = u32(d["ctl"], 0x700) if len(d["ctl"]) >= 0x704 else 0
        code = d["code"][:c_off] if 0 < c_off <= len(d["code"]) else d["code"]  # the instructions: a CTL_DUMP .code ends at the constants, archives at code_len
        h = hashlib.sha1(d["ctl"] + code).hexdigest()[:16]
        if h in self.seen:
            return
        self.seen.add(h)
        base = os.path.join(self.out, "%s_%s_%s" % (self.tag, h, CORPUS_STAGE[d["stage"]]))
        with open(base + ".bin", "wb") as f:
            f.write(d["code"])
        if d["ctl"]:
            with open(base + ".ctl", "wb") as f:
                f.write(d["ctl"])
        self.written[d["stage"]] = self.written.get(d["stage"], 0) + 1
        del d["code"], d["ctl"]


def scan_blob(data, name, progs, stats, inflate_all, depth=0, corpus=None):
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
            scan_blob(f, "%s/#%d" % (name, i), progs, stats, inflate_all or bool(SHADERISH.search(name)), depth + 1, corpus)
        return
    got = list(find_sections(data)) if len(data) >= 0x760 else []
    if got:
        stats["files_with_sections"] = stats.get("files_with_sections", 0) + 1
        if corpus is not None:
            pair_code(data, got, stats)
    for d in got:
        d["file"] = name
        if d.get("area"):  # a code area without a section (BAHS): corpus only
            corpus.add(d)
            continue
        if corpus is not None:
            if "code" not in d and not d.get("by_area"):
                u = stats.setdefault("unpaired_by_file", {})
                u[name] = u.get(name, 0) + 1
            corpus.add(d)
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


def scan_romfs(path, inflate_all=True, label=None, corpus=None):
    """Scan every file of a RomFS image for control sections (archives and compression opened). Returns (progs, stats)."""
    progs, stats = [], {"files": 0, "skipped_large": 0}
    label = label or os.path.basename(path)
    with open(path, "rb") as fh, mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ) as m:
        for name, off, size in romfs_entries(m):
            stats["files"] += 1
            if size > ROMFS_MAX_FILE or off + size > len(m):
                stats["skipped_large"] += 1
                continue
            scan_blob(m[off:off + size], label + ":" + name, progs, stats, inflate_all, 0, corpus)
    return progs, stats


def scan_dir(path, inflate_all=True, stats=None, corpus=None):
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
                    if corpus is not None:  # NAPPLE_GFX_CTL_DUMP: the code area sits next to it
                        try:
                            with open(fp[:-4] + ".code", "rb") as fh:
                                d["code"], d["ctl"] = fh.read(), data[:d["length"]]
                        except OSError:
                            pass
                        corpus.add(d)
                    progs.append(d)
                continue
            if f.endswith((".code", ".json")) or len(data) < 0x10:
                continue
            scan_blob(data, os.path.relpath(fp, path), progs, stats, inflate_all, 0, corpus)
    return progs, stats.get("files_with_sections", 0)


def scan_paths(paths, inflate_all=True, corpus=None):
    """scan_dir / scan_romfs over several inputs (a title's base, update and DLC). Returns (progs, stats)."""
    progs, stats = [], {}
    for p in paths:
        if os.path.isdir(p):
            pp, _ = scan_dir(p, True, stats, corpus)
            for d in pp:
                d["file"] = os.path.basename(os.path.abspath(p)) + ":" + d["file"]
        elif is_romfs(p):
            pp, st = scan_romfs(p, inflate_all, None, corpus)
            for k, v in st.items():
                if isinstance(v, dict):
                    stats.setdefault(k, {}).update(v)
                else:
                    stats[k] = stats.get(k, 0) + v
        else:
            pp = []
            with open(p, "rb") as fh:
                scan_blob(fh.read(), os.path.basename(p), pp, stats, True, 0, corpus)
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
    e = sub.add_parser("extract"); e.add_argument("dir", nargs="+"); e.add_argument("-o", "--out", required=True); e.add_argument("--tag", required=True)
    e.add_argument("--quick", action="store_true", help="as for scan")
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
    if a.cmd == "extract":
        corpus = Corpus(a.out, a.tag)
        progs, stats = scan_paths(a.dir, not a.quick, corpus)
        stats.update({"written_" + k: v for k, v in corpus.written.items()})
        stats["distinct_written"] = len(corpus.seen)
        print(json.dumps(stats, indent=2, sort_keys=True))
        sys.stderr.write("%s: %d sections, %d distinct programs written to %s (%s); %d sections without code\n" % (
            a.tag, len(progs), len(corpus.seen), a.out, ", ".join("%s=%d" % kv for kv in sorted(corpus.written.items())),
            stats.get("unpaired", 0)))
        return 0
    res = cover([json.load(open(p)) for p in a.scans])
    print(json.dumps(res, indent=2))
    sys.stderr.write("smallest set: %s\ngreedy: %s\nno title covers: %s\n" % (res["exact"], res["greedy"], ", ".join(res["features_no_title_covers"]) or "-"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
