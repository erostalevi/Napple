#!/usr/bin/env python3
"""Napple decompiler coverage scan: measures what the pipeline + runtime implement against fixed Switch (NSW)
reference catalogs. Game-agnostic by design: nothing here looks at knownSignatures, compat/matrix.json or per-title notes.

Usage: scan.py [--json] [--notes N]                       raw scan (what the code says, before judgement)
       scan.py --scoreboard --judgement FILE                standardized report (the one to show the user), nothing written
       scan.py --save --judgement FILE                      same, and write history/<date>-<sha>.json (the baseline for the next run)
       scan.py --compare [OLD [NEW]]                        diff two snapshots (default: the last two; with one arg: that one vs the latest)
Judgements made after reading the code live in verified.json next to this script and override measured statuses.
  --json   print the raw result as JSON instead of the report
  --judgement FILE  JSON written by the skill after verifying: {"adjustments": {"<domain key>": {"delta": -0.05, "reason": "..."}},
                    "priorities": [{"item": "<name>", "domain": "<key>", "importance": 1-3, "effort": "S|M|L", "why": "...", "where": "..."}]}
                    delta is a ratio change, clamped to +-0.15; final % and its range are computed here, never by hand.
  --notes  how many recent docs/archive/changes notes to excerpt (default 6)

Statuses: impl = 1.0, stub = 0.5, missing = 0. Tier weights: core 3, common 2, rare 1.
Keyword probes (domains marked "probe") only prove that a word appears in the code; the skill verifies each hit.
"""
import datetime
import json
import re
import subprocess
import sys
from pathlib import Path

SKILL = Path(__file__).resolve().parent
REPO = SKILL.parents[2]
P = REPO / "Napple" if (REPO / "Napple" / "runtime").is_dir() else REPO
TIER_W = {"core": 3, "common": 2, "rare": 1}
TIER_TAG = {"core": "core", "common": "com", "rare": "rare"}
STATUS_W = {"impl": 1.0, "partial": 0.75, "stub": 0.5, "missing": 0.0}

# ---- reference catalogs (Horizon / Tegra X1 knowledge, not per game) -------------------------------------------

# Horizon svcs reachable from an application process. System-only svcs (process management, debug, IO mapping) are left out.
SVC_TIERS = {
    "core": [0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12,
             0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x21, 0x24, 0x25, 0x26, 0x27,
             0x29, 0x34, 0x35],
    "common": [0x22, 0x28, 0x2A, 0x2B, 0x2C, 0x2D, 0x30, 0x31, 0x32, 0x36, 0x37, 0x45, 0x51, 0x52],
    "rare": [0x20, 0x23, 0x2E, 0x2F, 0x33, 0x40, 0x41, 0x43, 0x4B, 0x4C, 0x50],
}

# Application-facing IPC services. Each entry: label, alternatives (any registered name counts).
SERVICES = {
    "core": [("sm:", ["sm:"]), ("fsp-srv", ["fsp-srv"]), ("appletOE", ["appletOE"]), ("hid", ["hid"]),
             ("vi", ["vi:u", "vi:m", "vi:s"]), ("nvdrv", ["nvdrv", "nvdrv:a", "nvdrv:s", "nvdrv:t"]),
             ("set", ["set"]), ("set:sys", ["set:sys"]), ("time", ["time:u", "time:a", "time:s"]), ("acc:u0", ["acc:u0"]),
             ("apm", ["apm"]), ("audren:u", ["audren:u"]), ("audout:u", ["audout:u"]), ("pl:u", ["pl:u"]), ("lm", ["lm"]),
             ("pctl", ["pctl", "pctl:s", "pctl:r", "pctl:a"]), ("aoc:u", ["aoc:u"]), ("nifm:u", ["nifm:u"]),
             ("friend:u", ["friend:u"]), ("prepo", ["prepo:u", "prepo:a", "prepo:s", "prepo:m"]), ("csrng", ["csrng"]),
             ("hwopus", ["hwopus"])],
    "common": [("bsd:u", ["bsd:u"]), ("ssl", ["ssl"]), ("nsd", ["nsd:u", "nsd:a"]), ("sfdnsres", ["sfdnsres"]),
               ("nfp:user", ["nfp:user"]), ("mii:u", ["mii:u", "mii:e"]), ("caps:su", ["caps:su"]), ("caps:u", ["caps:u", "caps:a"]),
               ("ldn:u", ["ldn:u"]), ("psm", ["psm"]), ("lbl", ["lbl"]), ("irs", ["irs"]), ("bcat:u", ["bcat:u", "bcat:a"]),
               ("audin:u", ["audin:u"]), ("ro:1 / ldr:ro", ["ro:1", "ldr:ro"]), ("mm:u", ["mm:u"]), ("ncm", ["ncm"]),
               ("ns:am2 / ns:ec", ["ns:am2", "ns:ec", "ns:su"]), ("spsm", ["spsm"])],
    "rare": [("audrec:u", ["audrec:u"]), ("hidbus", ["hidbus"]), ("olsc:u", ["olsc:u"]), ("ectx:aw", ["ectx:aw"]),
             ("pdm:qry", ["pdm:qry"]), ("fgm", ["fgm"]), ("nfc:user", ["nfc:user"]), ("set:fd", ["set:fd"]),
             ("btm:u", ["btm:u"]), ("ovln:snd", ["ovln:snd"]), ("es", ["es"]), ("ns:vm", ["ns:vm"])],
}

# Maxwell SM5x shader ISA (NVN shaders are Maxwell binaries). Pattern defaults to the mnemonic as a word.
MAXWELL = {
    "core": ["ALD", "AST", "IPA", "MOV", "MOV32I", "FADD", "FADD32I", "FMUL", "FMUL32I", "FFMA", "FFMA32I", "FMNMX",
             "FSETP", "FSET", "FCMP", "MUFU", "RRO", "F2F", "F2I", "I2F", "I2I", "IADD", "IADD3", "IADD32I", "ISCADD",
             "ISETP", "ISET", "IMNMX", "SHL", "SHR", "LOP", "LOP3", "LOP32I", "BFE", "BFI", "XMAD", "SEL", "PSETP", "PSET",
             "TEX", "TEXS", "TLD", "TLDS", "TLD4", "TLD4S", "TXQ", "TMML", "LDC", "LDG", "STG", "LDL", "STL", "LDS", "STS",
             "EXIT", "KIL", "BRA", "SSY", "SYNC", "PBK", "BRK", "S2R", "HADD2", "HMUL2", "HFMA2", "HSETP2", "PRMT", "FLO",
             "POPC", "SHF", "DEPBAR", "NOP", "MEMBAR", "BAR"],
    "common": ["CONT", "PCNT", "BRX", "CSET", "CSETP", "P2R", "R2P", "IMUL", "IMUL32I", "IMAD", "ICMP", "ISCADD32I",
               "SHFL", "VOTE", "ATOM", "ATOMS", "RED", "FSWZADD", "TXD", "HSET2", "HADD2_32I", "HMUL2_32I", "HFMA2_32I",
               "LEA", "OUT", "SUST", "SULD", "SUATOM", "AL2P", "DADD", "DMUL", "DFMA", "DSETP", "DMNMX", "CS2R"],
    "rare": ["VMAD", "VADD", "VSET", "VSETP", "VMNMX", "VABSDIFF", "VABSDIFF4", "VSHL", "VSHR", "IDE", "IDP", "IMADSP",
             "JMP", "JMX", "CAL", "JCAL", "RET", "PRET", "PEXIT", "LONGJMP", "PLONGJMP", "B2R", "R2B", "CCTL", "CCTLL",
             "BPT", "RAM", "SAM", "RTT", "FCHK", "PIXLD", "ISBERD", "LEPC", "GETCRSPTR", "SETCRSPTR", "GETLMEMBASE",
             "SETLMEMBASE", "STP", "SURED", "TXA", "DSET", "IMAD32I"],
}
MAXWELL_ALIAS = {"ALD": r"\bALD\b|LD attribute", "AST": r"\bAST\b|ST attribute", "LDC": r"\bLDC\b|LD c\[",
                 "HADD2_32I": r"HADD2[^\n]*32I", "HMUL2_32I": r"HMUL2[^\n]*32I", "HFMA2_32I": r"HFMA2[^\n]*32I"}

# Keyword probes: (label, tier, regex, dirs relative to Napple/). Case-insensitive.
PROBES = {
    "content": [
        ("XCI (HFS0)", "core", r"\bxci\b|hfs0", ["crates"]),
        ("NSP (PFS0)", "core", r"\bpfs0\b", ["crates"]),
        ("NCA3 + AES-XTS header", "core", r"nca3", ["crates"]),
        ("NCA2 / NCA0", "rare", r"nca2|nca0", ["crates"]),
        ("titlekey (rights id) crypto", "core", r"rights_?id|titlekey", ["crates"]),
        ("personalized tickets", "rare", r"personali[sz]ed", ["crates"]),
        ("NSZ / XCZ (zstd)", "common", r"\bnsz\b|\bxcz\b|zstd", ["crates"]),
        ("BKTR update patching", "core", r"bktr", ["crates"]),
        ("CNMT metadata", "core", r"cnmt", ["crates"]),
        ("NACP control data", "core", r"nacp", ["crates"]),
        ("NPDM / ACID / ACI0", "core", r"npdm", ["crates", "runtime/src"]),
        ("RomFS (IVFC)", "core", r"ivfc|romfs", ["crates"]),
        ("IVFC hash-tree verification", "rare", r"verify_ivfc|ivfc.*(verif|hash)", ["crates"]),
        ("DLC (AddOnContent) mounting", "core", r"add_?on_?content|aoc", ["crates", "runtime/src/services/fs.c"]),
    ],
    "loader": [
        ("NSO segments (LZ4)", "core", r"lz4", ["crates"]),
        ("MOD0 / dynamic relocations", "core", r"mod0|r_aarch64_relative|relocs?", ["crates", "runtime/src"]),
        ("multi-module (rtld, sdk, subsdkN)", "core", r"subsdk", ["crates", "runtime/src"]),
        ("NRO runtime loading (ro:1)", "common", r"\bnro\b|nro0", ["crates", "runtime/src"]),
        ("host overrides of guest imports", "common", r"host_symbols_register", ["runtime/src"]),
    ],
    "rewrite_extra": [
        ("exclusive monitor windows (LDXR/STXR)", "core", r"exclusive", ["crates/napple-rewrite/src"]),
        ("jump tables / computed branches", "core", r"jumptab|jump table", ["crates/napple-rewrite/src"]),
        ("4K->16K page granularity", "core", r"16\s?k|0x4000|page_size|16384", ["crates/napple-rewrite/src", "runtime/src"]),
        ("per-site verification", "common", r"verify|check_site|fn check", ["crates/napple-rewrite/src"]),
        ("code memory / JIT (CreateCodeMemory)", "rare", r"codememory|code_memory", ["runtime/src"]),
    ],
    "gpu_fixed": [
        ("RGBA8 / BGRA8 (+sRGB)", "core", r"rgba8|bgra8", ["gfx/src"]),
        ("BC1-BC3", "core", r"\bbc1\b|bc1_|dxt1", ["gfx/src"]),
        ("BC4/BC5", "core", r"\bbc4|\bbc5|rgtc", ["gfx/src"]),
        ("BC6H/BC7", "core", r"bc6|bc7|bptc", ["gfx/src"]),
        ("ASTC (all block sizes)", "core", r"astc", ["gfx/src"]),
        ("depth/stencil formats", "core", r"d24s8|depth24|d32f|depth32", ["gfx/src"]),
        ("float formats (RGBA16F/32F, R11G11B10F)", "core", r"rgba16f|rgba32f|r11g11b10", ["gfx/src"]),
        ("block-linear swizzle", "core", r"block.?linear|deswizzle", ["gfx/src"]),
        ("transform feedback", "common", r"transform.?feedback|xfb", ["gfx/src"]),
        ("geometry shaders", "common", r"geometry", ["gfx/src"]),
        ("tessellation", "common", r"tessell", ["gfx/src"]),
        ("compute dispatch", "core", r"dispatch|compute", ["gfx/src"]),
        ("queries / counters (occlusion, timestamp)", "common", r"occlusion|timestamp|nvncounter|reportcounter", ["gfx/src"]),
        ("indirect draw/dispatch", "common", r"indirect", ["gfx/src"]),
        ("MSAA / resolve", "common", r"msaa|samples|resolve", ["gfx/src"]),
        ("texture views / sparse", "rare", r"sparse", ["gfx/src"]),
        ("shader cache (persisted MSL/pipelines)", "common", r"shader.?cache|pipeline.?cache|binary.?archive", ["gfx/src"]),
    ],
    "nvdrv": [
        ("/dev/nvmap", "core", r"/dev/nvmap", ["runtime/src"]),
        ("/dev/nvhost-ctrl", "core", r"/dev/nvhost-ctrl\b|nvhost-ctrl\"", ["runtime/src"]),
        ("/dev/nvhost-nvdec, vic, nvjpg, msenc", "common", r"nvhost-nvdec", ["runtime/src"]),
        ("/dev/nvhost-gpu, as-gpu, ctrl-gpu (only if NVN is bypassed)", "rare", r"nvhost-gpu|nvhost-as-gpu|nvhost-ctrl-gpu", ["runtime/src"]),
        ("/dev/nvdisp-disp0", "rare", r"nvdisp", ["runtime/src"]),
    ],
    "audio": [
        ("audren voices PCM16", "core", r"pcm16|pcm_?int16", ["audio/src"]),
        ("ADPCM", "core", r"adpcm", ["audio/src"]),
        ("final mix / submix", "core", r"submix|sub_mix|final_?mix", ["audio/src"]),
        ("splitter", "common", r"splitter", ["audio/src"]),
        ("delay", "core", r"delay", ["audio/src"]),
        ("reverb", "core", r"reverb", ["audio/src"]),
        ("I3DL2 reverb", "common", r"i3dl2", ["audio/src"]),
        ("aux send/return", "common", r"\baux", ["audio/src"]),
        ("biquad filter", "core", r"biquad", ["audio/src"]),
        ("limiter / light limiter", "common", r"limiter", ["audio/src"]),
        ("compressor", "rare", r"compressor", ["audio/src"]),
        ("capture effect", "rare", r"capture", ["audio/src"]),
        ("performance metrics", "common", r"performance", ["audio/src"]),
        ("audout", "core", r"audout", ["audio/src"]),
        ("audin (microphone)", "rare", r"audin", ["audio/src", "runtime/src/services"]),
        ("hwopus multistream", "common", r"multistream", ["audio/src", "runtime/src/services"]),
    ],
    "input": [
        ("Pro / FullKey", "core", r"fullkey|full_key", ["input/src", "runtime/src/services/hid.c"]),
        ("Handheld", "core", r"handheld", ["input/src", "runtime/src/services/hid.c"]),
        ("Joy-Con dual / left / right", "core", r"joydual|joy_dual|joyleft|joy_left", ["input/src", "runtime/src/services/hid.c"]),
        ("GameCube / Palma / NES styles", "rare", r"palma|\bgc\b|lark", ["input/src", "runtime/src/services/hid.c"]),
        ("six-axis motion", "core", r"sixaxis|six_axis", ["input/src", "runtime/src/services/hid.c"]),
        ("touchscreen", "core", r"touch", ["input/src", "runtime/src/services/hid.c"]),
        ("mouse / keyboard (HID)", "common", r"mouse", ["input/src", "runtime/src/services/hid.c"]),
        ("vibration", "core", r"vibration", ["input/src", "runtime/src/services/hid.c"]),
        ("npad assignment / hold type / multi-player", "common", r"assignment|hold_?type|npadjoy", ["input/src", "runtime/src/services/hid.c"]),
        ("gesture / debugpad", "rare", r"gesture|debugpad", ["input/src", "runtime/src/services/hid.c"]),
        ("IR camera (irs)", "rare", r"\birs\b|ircamera", ["input/src", "runtime/src/services"]),
    ],
    "applets": [
        ("swkbd (software keyboard)", "core", r"swkbd|softwarekeyboard", ["runtime/src"]),
        ("controller applet", "core", r"controller.?applet|controllersupport", ["runtime/src"]),
        ("error applet", "common", r"error.?applet", ["runtime/src"]),
        ("player select (profile)", "common", r"playerselect|player.?select|profile.?select", ["runtime/src"]),
        ("web / offline web", "rare", r"web.?applet|offline.?web|libappletweb", ["runtime/src"]),
        ("mii edit", "rare", r"mii.?edit", ["runtime/src"]),
        ("photo viewer / album", "rare", r"photo.?viewer|album", ["runtime/src"]),
    ],
    "media": [
        ("VP9 (host decoder)", "core", r"vp9|libvpx|vpx_", ["runtime/src"]),
        ("H.264", "core", r"h\.?264|avc", ["runtime/src"]),
        ("Vorbis/Opus movie audio", "common", r"vorbis|opus", ["runtime/src"]),
        ("JPEG (nvjpg / caps)", "rare", r"jpeg|nvjpg", ["runtime/src", "gfx/src"]),
    ],
    "fs_runtime": [
        ("RomFS as IStorage", "core", r"romfs", ["runtime/src/services/fs.c"]),
        ("transactional save data", "core", r"commit", ["runtime/src/services/fs.c"]),
        ("cache storage", "common", r"cache", ["runtime/src/services/fs.c"]),
        ("bcat storage", "rare", r"bcat", ["runtime/src/services/fs.c"]),
        ("temporary storage", "common", r"temporary", ["runtime/src/services/fs.c"]),
        ("sdcard", "rare", r"sdcard|sd_card", ["runtime/src/services/fs.c"]),
        ("save data extra data / size queries", "common", r"extradata|extra_data", ["runtime/src/services/fs.c"]),
    ],
}

REWRITE_KINDS = {"core": ["svc", "tpidr_r", "tpidr_w", "tpidrro_r", "cntpct", "cntvct", "cntfrq", "x18"]}

# Domain weights (sum 100). Packaging, Studio, launcher and per-game data are not Switch coverage and are left out.
DOMAINS = [
    ("cpu_rewrite", "CPU: AOT A64 rewrite", 14),
    ("loader", "Loader: NSO/NRO/modules/relocs", 5),
    ("kernel", "Kernel: Horizon svcs + scheduling", 10),
    ("ipc", "IPC: HLE services (coverage)", 10),
    ("ipc_depth", "IPC: command depth (impl vs stub)", 5),
    ("nvn", "GPU: NVN API surface", 12),
    ("shader", "GPU: Maxwell shader ISA", 12),
    ("gpu_fixed", "GPU: formats + pipeline features", 8),
    ("nvdrv", "GPU: nvdrv / host1x devices", 2),
    ("audio", "Audio: audren/audout/opus", 7),
    ("input", "Input: hid / npad", 5),
    ("content", "Content: containers, crypto, updates, DLC", 5),
    ("fs_runtime", "FS: saves + storage", 2),
    ("applets", "Library applets", 2),
    ("media", "Video / media decode", 1),
]


def read(p):
    try:
        return p.read_text(errors="replace")
    except OSError:
        return ""


def files_under(rel_dirs, exts=(".c", ".m", ".h", ".rs", ".S", ".mm", ".cpp")):
    out = []
    for rel in rel_dirs:
        base = P / rel
        if base.is_file():
            out.append(base)
        elif base.is_dir():
            out += [f for f in base.rglob("*") if f.suffix in exts and not re.search(r"/(build|target|tests?)/", str(f))]
    return out


def first_hit(regex, rel_dirs):
    rx = re.compile(regex, re.I)
    comment = re.compile(r"^\s*(//|/\*|\*|#)")
    n, where = 0, None
    for f in files_under(rel_dirs):
        for i, line in enumerate(read(f).splitlines(), 1):
            if rx.search(line) and not comment.match(line):
                n += 1
                if where is None:
                    where = f"{f.relative_to(P)}:{i}"
    return n, where


def score(items):
    """items: list of dicts with tier and status -> weighted ratio."""
    tot = sum(TIER_W[i["tier"]] for i in items)
    got = sum(TIER_W[i["tier"]] * STATUS_W.get(i["status"], 0.0) for i in items)
    return (got / tot) if tot else 0.0


# ---- measurements -------------------------------------------------------------------------------------------------

def m_kernel():
    src = "".join(read(f) for f in (P / "runtime/src").glob("*.c"))
    reg = {int(m, 16) for m in re.findall(r"svc_register\(\s*(0x[0-9a-fA-F]+)", src)}
    names = dict((int(a, 16), b) for a, b in re.findall(r"\[(0x[0-9a-fA-F]+)\]\s*=\s*\"(\w+)\"", read(P / "runtime/src/kernel.c")))
    items = [{"name": f"0x{n:02X} {names.get(n, '?')}", "tier": t, "status": "impl" if n in reg else "missing"}
             for t, nums in SVC_TIERS.items() for n in nums]
    return items, {"registered": len(reg)}


def m_ipc():
    sdir = P / "runtime/src/services"
    src = "".join(read(f) for f in sdir.glob("*.c")) + read(P / "runtime/src/ipc.c")
    lits = set(re.findall(r"\"([A-Za-z0-9:\-]+)\"", src))
    items = [{"name": lab, "tier": t, "status": "impl" if any(a in lits for a in alts) else "missing"}
             for t, lst in SERVICES.items() for lab, alts in lst]
    impl = len(re.findall(r"IPC_IMPL\s*\}", src))
    stub = len(re.findall(r"IPC_STUB\s*\}", src))
    per_file = {}
    for f in sorted(sdir.glob("*.c")):
        s = read(f)
        per_file[f.name] = [len(re.findall(r"IPC_IMPL\s*\}", s)), len(re.findall(r"IPC_STUB\s*\}", s))]
    offline = sorted({lab for lab, alts in sum(SERVICES.values(), []) if any(a in lits for a in alts)} &
                     {"friend:u", "nifm:u", "bsd:u", "nsd", "ssl", "prepo", "nfp:user", "nfc:user"})
    return items, {"cmd_impl": impl, "cmd_stub": stub, "per_file": per_file, "offline_stub_services": offline}


# IPC depth against the reference lists (runtime/tests/ipc_reference/*.txt, switchbrew/libnx ids with an app_reachable flag). The binding of
# command tables to interface names is the runner's (runtime/tests/test_ipc_reference.c), compiled here with a small wrapper that also dumps
# every table and binding, so the scan and `make test` can never disagree on which row answers which command.
IPCREF_WRAPPER = r'''
#define main ipcref_main
#include "%s"
#undef main
int main(int argc, char **argv) {
    int rc = ipcref_main(argc, argv);
    for (unsigned t = 0; t < ntables; t++) {
        unsigned st = 0;
        for (unsigned r = 0; r < tables[t].nrows; r++) st += tables[t].rows[r].stub != 0;
        printf("@TABLE\t%%u\t%%s\t%%s\t%%u\t%%u\n", t, tables[t].file, tables[t].ident, tables[t].nrows - st, st);
    }
    for (unsigned b = 0; b < nbinds; b++) printf("@BIND\t%%s\t%%u\n", binds[b].iface, binds[b].table);
    printf("@RC\t%%d\n", rc);
    return 0;
}
'''


def m_ipc_depth(fallback_impl, fallback_stub):
    """Reference-based depth: every app-reachable id of a fixture scores impl 1 / stub 0.5 / missing 0 (STATUS_W, as every other
    catalog); command tables bound to no fixture (e.g. the am applet interfaces) still count row by row. Falls back to the old
    row ratio (impl / all rows) when the runner cannot be compiled."""
    import tempfile
    rt = P / "runtime"
    src, fixdir = rt / "tests/test_ipc_reference.c", rt / "tests/ipc_reference"
    fb = {"method": "rows (reference runner unavailable)", "impl": fallback_impl, "stub": fallback_stub}
    if not src.is_file() or not fixdir.is_dir():
        return fallback_impl / max(1, fallback_impl + fallback_stub), fb
    try:
        with tempfile.TemporaryDirectory() as tmp:
            w = Path(tmp) / "ipcref_scan.c"
            w.write_text(IPCREF_WRAPPER % str(src))
            exe = Path(tmp) / "ipcref_scan"
            subprocess.run(["cc", "-O1", "-w", "-o", str(exe), str(w)], check=True, capture_output=True, timeout=120)
            out = subprocess.run([str(exe), str(rt)], capture_output=True, text=True, timeout=120).stdout
    except Exception:
        return fallback_impl / max(1, fallback_impl + fallback_stub), fb
    rx = re.compile(r"^(\S+)\s+\d+ ids .*\|\s*(\d+) app-reachable:\s+(\d+) with rows \((\d+) impl, (\d+) stub\), missing (\d+)")
    ifaces, tables, binds = {}, {}, []
    for line in out.splitlines():
        m = rx.match(line)
        if m:
            name, app, _, impl, stub, miss = m.group(1), *map(int, m.groups()[1:])
            ifaces[name] = {"app": app, "impl": impl, "stub": stub, "missing": miss}
        elif line.startswith("@TABLE\t"):
            _, t, f, ident, impl, stub = line.split("\t")
            tables[int(t)] = {"file": f, "ident": ident, "impl": int(impl), "stub": int(stub)}
        elif line.startswith("@BIND\t"):
            _, iface, t = line.split("\t")
            binds.append((iface, int(t)))
    if not ifaces or not tables:
        return fallback_impl / max(1, fallback_impl + fallback_stub), fb
    covered_names, covered_tables = set(), set()
    for f in fixdir.glob("*.txt"):
        for line in read(f).splitlines():
            if not line.startswith("#"):
                continue
            m = re.match(r"#\s*interface\s+(\S+)", line) or re.match(r"#\s*alias\s+(\S+)", line)
            if m:
                covered_names.add(m.group(1))
            m = re.match(r"#\s*table\s+(\S+):(\w+)", line)
            if m:
                covered_tables |= {t for t, d in tables.items() if d["file"] == m.group(1) and d["ident"] == m.group(2)}
    covered_tables |= {t for iface, t in binds if iface in covered_names}
    bound = {t for _, t in binds} | covered_tables
    unfixtured = [d for t, d in tables.items() if t not in covered_tables and t in bound]
    app = sum(v["app"] for v in ifaces.values())
    impl = sum(v["impl"] for v in ifaces.values())
    stub = sum(v["stub"] for v in ifaces.values())
    miss = sum(v["missing"] for v in ifaces.values())
    u_impl, u_stub = sum(d["impl"] for d in unfixtured), sum(d["stub"] for d in unfixtured)
    tot = app + u_impl + u_stub
    got = impl * STATUS_W["impl"] + stub * STATUS_W["stub"] + u_impl * STATUS_W["impl"] + u_stub * STATUS_W["stub"]
    by_file = {}
    for d in unfixtured:
        by_file.setdefault(d["file"], [0, 0])
        by_file[d["file"]][0] += d["impl"]
        by_file[d["file"]][1] += d["stub"]
    most_stubbed = sorted(((n, v["stub"] + v["missing"], v["app"]) for n, v in ifaces.items() if v["stub"] + v["missing"]),
                          key=lambda x: -x[1])[:12]
    return got / max(1, tot), {
        "method": "reference (tests/ipc_reference app-reachable ids; impl 1, stub 0.5, missing 0) + unfixtured tables row by row",
        "fixtures": len(ifaces), "app_reachable": app, "ref_impl": impl, "ref_stub": stub, "ref_missing": miss,
        "unfixtured_impl": u_impl, "unfixtured_stub": u_stub, "unfixtured_by_file": dict(sorted(by_file.items())),
        "most_stubbed_interfaces": most_stubbed, "impl": impl + u_impl, "stub": stub + u_stub, "missing": miss,
        "row_ratio_old": round(fallback_impl / max(1, fallback_impl + fallback_stub), 4)}


def m_nvn():
    names = [l.strip() for l in read(P / "gfx/src/nvnapi/names.txt").splitlines() if l.strip()]
    src = "".join(read(f) for f in files_under(["gfx/src"]))
    defined = set(re.findall(r"^NVN(?:_NOPROTO)?\((nvn\w+)", src, re.M))
    stubby = set()
    for m in re.finditer(r"^NVN(?:_NOPROTO)?\((nvn\w+)[^{]*\{(.*?)\}\s*$", src, re.M):
        body = m.group(2)
        if not body.strip() or re.search(r"stub|todo|unimpl|not implemented", body, re.I):
            stubby.add(m.group(1))
    # The whole NVN surface is one tier; the getters/setters that games call every frame are not distinguishable statically.
    items = [{"name": n, "tier": "core", "status": ("stub" if n in stubby else "impl") if n in defined else "missing"}
             for n in names]
    groups = {}
    for i in items:
        if i["status"] == "missing":
            g = re.match(r"nvn([A-Z][a-z]+(?:[A-Z][a-z]+)?)", i["name"])
            groups[g.group(1) if g else "?"] = groups.get(g.group(1) if g else "?", 0) + 1
    return items, {"catalog": len(names), "defined": len(defined & set(names)), "stub_like": len(stubby),
                   "missing_by_object": dict(sorted(groups.items(), key=lambda kv: -kv[1])[:15])}


def m_shader():
    src = read(P / "gfx/src/shader/maxwell_translate.c")
    comments = "\n".join(re.findall(r"//([^\n]*)", src))
    fails = re.findall(r"fail\(c, \"([^\"]+)\"", src)
    items = []
    for t, ops in MAXWELL.items():
        for op in ops:
            rx = MAXWELL_ALIAS.get(op, r"\b" + re.escape(op) + r"\b(?![._])")
            items.append({"name": op, "tier": t, "status": "impl" if re.search(rx, comments) else "missing"})
    return items, {"explicit_failures": sorted(set(fails))}


def m_rewrite():
    s = read(P / "crates/napple-rewrite/src/scan.rs")
    labels = set(re.findall(r"=>\s*\"(\w+)\"", s))
    items = [{"name": k, "tier": "core", "status": "impl" if k in labels else "missing"} for k in REWRITE_KINDS["core"]]
    extra, _ = m_probe("rewrite_extra")
    return items + extra, {}


def m_probe(domain):
    items = []
    for label, tier, rx, dirs in PROBES[domain]:
        n, where = first_hit(rx, dirs)
        items.append({"name": label, "tier": tier, "status": "impl" if n else "missing", "hits": n, "first": where})
    return items, {"probe": True}


def git(*args):
    try:
        return subprocess.run(["git", "-C", str(REPO), *args], capture_output=True, text=True, timeout=30).stdout
    except Exception:
        return ""


def recent_notes(limit, since=None):
    notes = sorted((P / "docs/archive/changes").glob("*.md"), key=lambda f: f.name, reverse=True)
    if since:
        notes = [n for n in notes if n.name[:10] >= since] or notes[:2]
    out = []
    keep = re.compile(r"not done|pending|risk|not yet|blocker|todo|missing|known issue|limitation", re.I)
    for f in notes[:limit]:
        lines, grab, buf = read(f).splitlines(), False, []
        for line in lines:
            if line.startswith("#"):
                grab = bool(keep.search(line))
                if grab:
                    buf.append(line)
                continue
            if grab and line.strip():
                buf.append(line[:240])
        out.append({"file": f"docs/archive/changes/{f.name}", "open_items": buf[:40]})
    return out


def todo_count():
    rx = re.compile(r"\b(TODO|FIXME|XXX|not implemented|unimplemented)\b", re.I)
    n = 0
    for f in files_under(["runtime/src", "gfx/src", "audio/src", "input/src", "crates"]):
        n += sum(1 for l in read(f).splitlines() if rx.search(l))
    return n


def run(notes_limit):
    measured = {
        "cpu_rewrite": m_rewrite(), "loader": m_probe("loader"), "kernel": m_kernel(), "ipc": m_ipc(),
        "nvn": m_nvn(), "shader": m_shader(), "gpu_fixed": m_probe("gpu_fixed"), "nvdrv": m_probe("nvdrv"),
        "audio": m_probe("audio"), "input": m_probe("input"), "content": m_probe("content"),
        "fs_runtime": m_probe("fs_runtime"), "applets": m_probe("applets"), "media": m_probe("media"),
    }
    # Judgements recorded by the skill after reading the code: verified.json {"<domain>/<item>": {"status", "note", "date"}}.
    verified = json.loads(read(SKILL / "verified.json") or "{}")
    for key, (items, _) in measured.items():
        for i in items:
            v = verified.get(f"{key}/{i['name']}")
            if v:
                i["status"], i["verified"] = v["status"], f"{v.get('date', '?')}: {v.get('note', '')}"
    ipc_extra = measured["ipc"][1]
    depth, depth_extra = m_ipc_depth(ipc_extra["cmd_impl"], ipc_extra["cmd_stub"])
    domains, overall = [], 0.0
    for key, title, w in DOMAINS:
        if key == "ipc_depth":
            ratio, items, extra = depth, [], depth_extra
        else:
            items, extra = measured[key]
            ratio = score(items)
        overall += w * ratio
        missing = [i for i in items if i["status"] != "impl"]
        missing.sort(key=lambda i: -TIER_W[i["tier"]])
        domains.append({"key": key, "title": title, "weight": w, "ratio": round(ratio, 4),
                        "points": round(w * ratio, 2), "counts": {
                            t: [sum(1 for i in items if i["tier"] == t and i["status"] == "impl"),
                                sum(1 for i in items if i["tier"] == t)] for t in TIER_W},
                        "missing": missing, "items": items, "extra": extra})
    hist = load_history()
    prev = hist[-1] if hist else None
    since = prev["date"] if prev else None
    return {
        "date": datetime.date.today().isoformat(),
        "commit": git("rev-parse", "--short", "HEAD").strip(),
        "dirty_files": len([l for l in git("status", "--porcelain").splitlines() if l.strip()]),
        "measured_percent": round(overall, 1),
        "domains": domains,
        "todo_markers": todo_count(),
        "previous": {"date": prev["date"], "measured_percent": prev["measured_percent"],
                     "final_percent": prev.get("final_percent"),
                     "domains": {d["key"]: d.get("judged_points", d["points"]) for d in prev["domains"]}} if prev else None,
        # The previous snapshot's commit is the exact baseline (a date-only --since is read by git as that day at the current time of day).
        "commits_since_previous": (git("log", "--oneline", f"{prev['commit']}..HEAD") if prev and prev.get("commit")
                                   and git("cat-file", "-t", prev["commit"]).strip() == "commit"
                                   else git("log", "--oneline", f"--since={since}T00:00" if since else "-15")).splitlines()[:40],
        "notes": recent_notes(notes_limit, since),
    }


def report(r):
    out = [f"# Napple coverage scan {r['date']} (commit {r['commit']}, {r['dirty_files']} uncommitted files)", "",
           f"Measured coverage (before judgement): {r['measured_percent']}%"]
    if r["previous"]:
        out.append(f"Previous run {r['previous']['date']}: measured {r['previous']['measured_percent']}%, "
                   f"final {r['previous'].get('final_percent')}%")
    out += ["", "| Domain | W | core | common | rare | ratio | pts | delta |", "|---|---|---|---|---|---|---|---|"]
    for d in r["domains"]:
        c = d["counts"]
        delta = ""
        if r["previous"] and d["key"] in r["previous"]["domains"]:
            delta = f"{d['points'] - r['previous']['domains'][d['key']]:+.2f}"
        cell = lambda t: f"{c[t][0]}/{c[t][1]}" if c[t][1] else "-"
        out.append(f"| {d['title']}{' (probe)' if d['extra'].get('probe') else ''} | {d['weight']} | {cell('core')} | "
                   f"{cell('common')} | {cell('rare')} | {d['ratio']:.0%} | {d['points']:.1f} | {delta} |")
    out += ["", "## Missing / stub items (core first)"]
    for d in r["domains"]:
        if not d["missing"]:
            continue
        if d["key"] == "nvn":
            out.append(f"- **{d['title']}**: {len(d['missing'])} of {d['extra']['catalog']} entry points not defined or "
                       f"stub-like; missing by object: {d['extra']['missing_by_object']}")
            continue
        names = [f"{i['name']}[{TIER_TAG[i['tier']]}{'' if i['status'] == 'missing' else '/' + i['status']}]" for i in d["missing"]]
        out.append(f"- **{d['title']}**: " + ", ".join(names))
    out += ["", "## Unverified probe items (a hit only means the word appears outside comments/tests; 0 hits is not proof of absence)"]
    for d in r["domains"]:
        if d["extra"].get("probe"):
            for i in d["items"]:
                if "verified" in i:
                    continue
                out.append(f"- {d['key']}/{i['name']}: {i['hits']} hits, first {i['first']}")
    ipc = next(d for d in r["domains"] if d["key"] == "ipc")["extra"]
    dep = next(d for d in r["domains"] if d["key"] == "ipc_depth")["extra"]
    out += ["", "## IPC detail", f"commands impl/stub per file: {ipc['per_file']}",
            f"offline-stubbed network services: {ipc['offline_stub_services']}",
            f"depth method: {dep.get('method', 'rows')}"]
    if "app_reachable" in dep:
        out += [f"reference: {dep['fixtures']} fixtures, {dep['app_reachable']} app-reachable ids: {dep['ref_impl']} impl, "
                f"{dep['ref_stub']} stub, {dep['ref_missing']} missing; unfixtured tables {dep['unfixtured_impl']} impl / "
                f"{dep['unfixtured_stub']} stub {dep['unfixtured_by_file']}; old row ratio {dep['row_ratio_old']:.0%}",
                f"most stubbed interfaces (stub+missing, of app-reachable): {dep['most_stubbed_interfaces']}"]
    sh = next(d for d in r["domains"] if d["key"] == "shader")["extra"]
    out += ["", "## Shader translator explicit failure paths", ", ".join(sh["explicit_failures"]) or "(none)"]
    out += ["", f"TODO/FIXME/not-implemented markers in sources: {r['todo_markers']}", "",
            "## Commits since previous run" if r["previous"] else "## Recent commits"]
    out += [f"- {c}" for c in r["commits_since_previous"]]
    out += ["", "## Open items in recent change notes"]
    for n in r["notes"]:
        out.append(f"### {n['file']}")
        out += n["open_items"] or ["(no open-items section)"]
    return "\n".join(out)



SCHEMA_VERSION = 2
EFFORT_W = {"S": 1, "M": 3, "L": 8}
MAX_ADJ = 0.15


def load_history():
    snaps = []
    for f in (SKILL / "history").glob("*.json"):
        try:
            d = json.loads(read(f))
        except ValueError:
            continue
        d["_file"] = f.name
        snaps.append(d)
    return sorted(snaps, key=lambda d: (d.get("date", ""), d.get("saved_at", ""), d["_file"]))


def judge(r, judgement):
    """Apply the skill's judgement to a raw scan: adjusted ratios, final %, range, ranked priorities. Deterministic."""
    adj = (judgement or {}).get("adjustments", {})
    total_u, final = 0.0, 0.0
    for d in r["domains"]:
        a = adj.get(d["key"], {})
        delta = max(-MAX_ADJ, min(MAX_ADJ, float(a.get("delta", 0))))
        d["adjust"], d["adjust_reason"] = round(delta, 3), a.get("reason", "")
        d["judged_ratio"] = round(max(0.0, min(1.0, d["ratio"] + delta)), 4)
        d["judged_points"] = round(d["weight"] * d["judged_ratio"], 2)
        unv = [i for i in d["items"] if d["extra"].get("probe") and "verified" not in i]
        tot = sum(TIER_W[i["tier"]] for i in d["items"]) or 1
        d["unverified"] = len(unv)
        d["uncertainty"] = round(d["weight"] * sum(TIER_W[i["tier"]] for i in unv) / tot / 2, 2)
        final += d["judged_points"]
        total_u += d["uncertainty"]
    r["final_percent"] = round(final, 1)
    r["range"] = [round(max(0.0, final - total_u), 1), round(min(100.0, final + total_u), 1)]
    r["unverified_total"] = sum(d["unverified"] for d in r["domains"])
    weights = {d["key"]: d["weight"] for d in r["domains"]}
    prios = []
    for p in (judgement or {}).get("priorities", []):
        score = int(p.get("importance", 1)) * weights.get(p.get("domain"), 1) / EFFORT_W.get(p.get("effort", "M"), 3)
        dom = next((d for d in r["domains"] if d["key"] == p.get("domain")), None)
        it = next((i for i in dom["items"] if i["name"] == p.get("item")), None) if dom else None
        gain = (dom["weight"] * TIER_W[it["tier"]] / (sum(TIER_W[i["tier"]] for i in dom["items"]) or 1)
                * (1 - STATUS_W[it["status"]])) if it else None   # coverage points closing this item would add (None = not a catalog item)
        prios.append({**p, "id": f"{p.get('domain')}/{p.get('item')}", "score": round(score, 2),
                      "gain_pts": round(gain, 2) if gain is not None else None})
    prios.sort(key=lambda p: -p["score"])
    for n, p in enumerate(prios, 1):
        p["rank"] = n
    r["priorities"] = prios
    r["judgement_notes"] = (judgement or {}).get("notes", "")
    return r


def snapshot(r):
    """Slim, stable snapshot: everything compare needs, nothing run-specific except identifiers."""
    slim = {k: v for k, v in r.items() if k not in ("notes", "commits_since_previous", "previous", "domains")}
    slim["schema"] = SCHEMA_VERSION
    slim["saved_at"] = datetime.datetime.now().isoformat(timespec="seconds")
    slim["domains"] = [{k: v for k, v in d.items() if k not in ("items", "missing")} for d in r["domains"]]
    slim["items"] = {d["key"]: {i["name"]: i["status"] for i in d["items"]} for d in r["domains"]}
    return slim


def sign(x):
    return f"{x:+.1f}"


def scoreboard(r):
    """The standardized report. Fixed section order, fixed columns, fixed number formats: runs are comparable by eye and by diff."""
    prev = r["previous"]
    pf = prev.get("final_percent") if prev else None
    out = [f"# Napple %Analysis - {r['date']} (commit {r['commit']}, {r['dirty_files']} uncommitted files)", "",
           f"SCORE final={r['final_percent']:.1f} range={r['range'][0]:.1f}-{r['range'][1]:.1f} measured={r['measured_percent']:.1f} "
           f"unverified={r['unverified_total']} todo_markers={r['todo_markers']} schema={SCHEMA_VERSION}", "",
           f"**Estimated NSW coverage: {r['final_percent']:.1f}% (range {r['range'][0]:.1f}-{r['range'][1]:.1f}%)**"
           + (f" - previous {prev['date']}: {pf:.1f}% ({sign(r['final_percent'] - pf)})" if pf is not None else " - no previous run"), "",
           "## Domains", "| Domain | Weight | Measured | Adj | Judged | Points | Delta | Note |", "|---|---|---|---|---|---|---|---|"]
    for d in r["domains"]:
        pd = prev["domains"].get(d["key"]) if prev else None
        delta = f"{d['judged_points'] - pd:+.2f}" if pd is not None else "n/a"
        note = (d["adjust_reason"] or "").replace("|", "/")
        out.append(f"| {d['title']} | {d['weight']} | {d['ratio']:.0%} | {d['adjust']:+.2f} | {d['judged_ratio']:.0%} | "
                   f"{d['judged_points']:.2f} | {delta} | {note} |")
    out += ["", "## Priorities (importance x weight / effort; S=1 M=3 L=8)", "| # | Item | Domain | Imp | Effort | Score | Gain pts | Where / why |", "|---|---|---|---|---|---|---|---|"]
    for p in r["priorities"][:10]:
        out.append(f"| {p['rank']} | {p.get('item')} | {p.get('domain')} | {p.get('importance')} | {p.get('effort')} | {p['score']:.2f} | "
                   f"{'n/a' if p['gain_pts'] is None else format(p['gain_pts'], '.2f')} | {(p.get('where', '') + ' - ' + p.get('why', '')).strip(' -')} |")
    if len(r["priorities"]) > 10:
        out.append(f"(+{len(r['priorities']) - 10} more in the snapshot)")
    out += ["", "## Since last run", *(f"- {c}" for c in r["commits_since_previous"][:6]), "", "## Confidence",
            f"Unverified probe items: {r['unverified_total']}. Adjustments: " +
            (", ".join(f"{d['key']} {d['adjust']:+.2f}" for d in r["domains"] if d["adjust"]) or "none") + "."]
    if r["judgement_notes"]:
        out.append(r["judgement_notes"])
    return "\n".join(out)


def compare(old, new):
    """Standardized diff of two snapshots (old/new are snapshot dicts)."""
    f = lambda d: d.get("final_percent")
    out = [f"# Napple %Analysis change {old['date']} ({old.get('commit', '?')}) -> {new['date']} ({new.get('commit', '?')})", "",
           f"CHANGE final {f(old):.1f} -> {f(new):.1f} ({f(new) - f(old):+.1f}) range {new['range'][0]:.1f}-{new['range'][1]:.1f}", "",
           "| Domain | Old pts | New pts | Delta |", "|---|---|---|---|"]
    od = {d["key"]: d.get("judged_points", d["points"]) for d in old["domains"]}
    for d in new["domains"]:
        n, o = d.get("judged_points", d["points"]), od.get(d["key"])
        out.append(f"| {d['title']} | {o if o is not None else 'n/a'} | {n:.2f} | {(n - o) if o is not None else 0:+.2f} |")
    oi, ni = old.get("items"), new.get("items")
    if oi and ni:
        closed, regress, added = [], [], []
        for dom, items in ni.items():
            for name, st in items.items():
                was = oi.get(dom, {}).get(name)
                if was is None:
                    added.append(f"{dom}/{name} [{st}]")
                elif was != st:
                    rank = {"missing": 0, "stub": 1, "partial": 2, "impl": 3}
                    (closed if rank[st] > rank[was] else regress).append(f"{dom}/{name}: {was} -> {st}")
        lst = lambda xs, n=None: [f"- {x}" for x in (xs[:n] if n else xs)] or ["(none)"]
        out += ["", f"## Gaps closed or improved ({len(closed)})", *lst(closed),
                "", f"## Regressions ({len(regress)})", *lst(regress),
                "", f"## Catalog items added ({len(added)})", *lst(added, 30)]
    else:
        out += ["", "(item-level diff unavailable: the older snapshot predates schema 2)"]
    ou, nu = old.get("unverified_total"), new.get("unverified_total")
    out += ["", f"Unverified items: {ou if ou is not None else 'n/a'} -> {nu if nu is not None else 'n/a'}; "
            f"TODO markers: {old.get('todo_markers')} -> {new.get('todo_markers')}"]
    op = {p["id"] for p in old.get("priorities", [])}
    np_ = {p["id"] for p in new.get("priorities", [])}
    if op or np_:
        out += [f"Priorities closed/dropped since old: {sorted(op - np_) or 'none'}", f"New priorities: {sorted(np_ - op) or 'none'}"]
    return "\n".join(out)


def main():
    args = sys.argv[1:]
    if "--compare" in args:
        rest = [a for a in args[args.index("--compare") + 1:] if not a.startswith("--")]
        hist = load_history()
        pick = lambda name: next((h for h in hist if h["_file"] in (name, name + ".json") or h["date"] == name), None)
        if len(rest) >= 2:
            old, new = pick(rest[0]), pick(rest[1])
        elif len(rest) == 1:
            old, new = pick(rest[0]), (hist[-1] if hist else None)
        else:
            old, new = (hist[-2], hist[-1]) if len(hist) >= 2 else (None, None)
        if not old or not new:
            sys.exit("compare: need two snapshots in history/ (available: " + ", ".join(h["_file"] for h in hist) + ")")
        print(compare(old, new))
        return
    limit = int(args[args.index("--notes") + 1]) if "--notes" in args else 6
    r = run(limit)
    if "--scoreboard" in args or "--save" in args:
        if "--judgement" not in args:
            sys.exit("--scoreboard/--save need --judgement FILE (write it after step 2-3 of the skill)")
        r = judge(r, json.loads(read(Path(args[args.index("--judgement") + 1]))))
        if "--save" in args:
            (SKILL / "history").mkdir(exist_ok=True)
            out = SKILL / "history" / f"{r['date']}-{r['commit']}.json"
            out.write_text(json.dumps(snapshot(r), indent=1))
        print(scoreboard(r))
        if "--save" in args:
            print(f"\nsaved {out.relative_to(REPO)}")
        return
    print(json.dumps(r, indent=1) if "--json" in args else report(r))


if __name__ == "__main__":
    main()
