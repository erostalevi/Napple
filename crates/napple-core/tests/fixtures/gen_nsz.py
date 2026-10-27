#!/usr/bin/env python3
"""Regenerates tests/fixtures/nsz/* with the public `nsz` tool (pip install nsz, 5.x) from SYNTHETIC NCAs.

Nothing here is a real key or real game data: every key is random (a few are forged to the CRC32
that nsz insists on), every NCA body is generated from a seed. The files are committed so the Rust
tests need neither Python nor network; run this only to regenerate them:

    python3 -m venv /tmp/nszvenv && /tmp/nszvenv/bin/pip install nsz==5.0.0
    /tmp/nszvenv/bin/python tests/fixtures/gen_nsz.py tests/fixtures/nsz

Outputs (*.bin so the repo's *.nsz / *.nca / *.keys ignore rules do not apply; the keys are synthetic): solid / block .nsz, an .xcz, first-section-at-0xC00
and at-0x4000 layouts, NCA2, and a rights-managed NCA whose ticket is rewritten to a PERSONALIZED
(RSA-OAEP) one after nsz ran.
"""
import hashlib
import os
import random
import struct
import sys
import zlib
from pathlib import Path

from Crypto.Cipher import AES
from Crypto.Hash import SHA256
from Crypto.PublicKey import RSA
from Crypto.Cipher import PKCS1_OAEP
from Crypto.Util import Counter

# Throwaway 2048-bit test modulus shared with src/infrastructure/ticket/ticket_testkey.rs
_tk = Path(__file__).resolve().parents[2] / "src/infrastructure/ticket/ticket_testkey.rs"
RSA_N = int(next(l for l in _tk.read_text().splitlines() if l.startswith("pub const N")).split('"')[1], 16)

# ---- synthetic keys -----------------------------------------------------------------------------
# nsz verifies a CRC32 of the master key and the *_source keys; random keys are forged to match.
CRC = {
    "aes_kek_generation_source": 2545229389,
    "aes_key_generation_source": 459881589,
    "titlekek_source": 3510501772,
    "key_area_key_application_source": 4130296074,
    "key_area_key_ocean_source": 3975316347,
    "key_area_key_system_source": 4024798875,
    "master_key_00": 3540309694,
}


def forge16(rng, target):
    """16 bytes: 12 random + 4 solved so that crc32 == target (CRC32 is affine over GF(2))."""
    pre = bytes(rng.randrange(256) for _ in range(12))

    def f(x):
        return zlib.crc32(pre + struct.pack("<I", x))

    base = f(0)
    cols = [f(1 << i) ^ base for i in range(32)]  # image of each basis bit
    want = target ^ base
    # Solve sum(x_i * cols[i]) = want over GF(2) by Gaussian elimination.
    rows = [[(cols[i] >> r) & 1 for i in range(32)] + [(want >> r) & 1] for r in range(32)]
    piv = []
    r = 0
    for c in range(32):
        p = next((k for k in range(r, 32) if rows[k][c]), None)
        if p is None:
            continue
        rows[r], rows[p] = rows[p], rows[r]
        for k in range(32):
            if k != r and rows[k][c]:
                rows[k] = [a ^ b for a, b in zip(rows[k], rows[r])]
        piv.append(c)
        r += 1
    x = 0
    for i, c in enumerate(piv):
        if rows[i][32]:
            x |= 1 << c
    out = pre + struct.pack("<I", x)
    assert zlib.crc32(out) == target
    return out


def make_keys(seed=1):
    rng = random.Random(seed)
    k = {n: forge16(rng, c) for n, c in CRC.items()}
    k["header_key"] = bytes(rng.randrange(256) for _ in range(32))
    return k


def ecb_enc(key, d):
    return AES.new(key, AES.MODE_ECB).encrypt(d)


def ecb_dec(key, d):
    return AES.new(key, AES.MODE_ECB).decrypt(d)


def derived(k):
    """hactool/nsz derivation of key_area_key_application_00 and titlekek_00."""
    m = k["master_key_00"]
    kek = ecb_dec(m, k["aes_kek_generation_source"])
    kak = ecb_dec(ecb_dec(kek, k["key_area_key_application_source"]), k["aes_key_generation_source"])
    return kak, ecb_dec(m, k["titlekek_source"])


def keys_text(k):
    return "".join(f"{n} = {v.hex()}\n" for n, v in sorted(k.items()))


# ---- NCA builder --------------------------------------------------------------------------------
def pad(b, to):
    return b + bytes(-len(b) % to)


def pfs0(files):
    strtab, ents, off = b"", b"", 0
    for n, d in files:
        ents += struct.pack("<QQII", off, len(d), len(strtab), 0)
        strtab += n.encode() + b"\0"
        off += len(d)
    strtab = pad(b"PFS0" + b"", 1)[:0] + strtab
    hdr = b"PFS0" + struct.pack("<III", len(files), len(strtab), 0) + ents + strtab
    return hdr + b"".join(d for _, d in files)


def hfs0(files):
    strtab, ents, off = b"", b"", 0
    for n, d in files:
        ents += struct.pack("<QQII", off, len(d), len(strtab), 0) + b"\0" * 8 + b"\0" * 32
        ents = ents  # 0x40-byte entries, hash fields zero
        strtab += n.encode() + b"\0"
        off += len(d)
    strtab = pad(strtab, 0x20)
    hdr = b"HFS0" + struct.pack("<III", len(files), len(strtab), 0) + ents + strtab
    return hdr + b"".join(d for _, d in files)


def xts_encrypt(key32, data, sector):
    """AES-128-XTS with the Nintendo tweak (big-endian sector number)."""
    k1, k2 = key32[:16], key32[16:]
    tweak = ecb_enc(k2, sector.to_bytes(16, "big"))
    out = b""
    t = int.from_bytes(tweak, "little")
    for i in range(0, len(data), 16):
        blk = int.from_bytes(data[i : i + 16], "little") ^ t
        c = ecb_enc(k1, blk.to_bytes(16, "little"))
        out += (int.from_bytes(c, "little") ^ t).to_bytes(16, "little")
        t <<= 1
        if t >> 128:
            t = (t & ((1 << 128) - 1)) ^ 0x87
    return out


def ctr_crypt(key, ctr_hi8_le, data, abs_off):
    iv = int.from_bytes(bytes(reversed(ctr_hi8_le)) + bytes(8), "big") + (abs_off >> 4)
    c = AES.new(key, AES.MODE_CTR, counter=Counter.new(128, initial_value=iv))
    return c.encrypt(data)


def body_bytes(rng, n, compressible=0.85):
    """Compressible text-like head plus a noise tail (so blocks include raw-stored ones)."""
    head = int(n * compressible)
    words = [b"main", b"nso", b"data", b"text", b"rodata", b"napple", b"synthetic", b"\x00\x00\x00\x00"]
    out = bytearray()
    while len(out) < head:
        out += rng.choice(words) + bytes([rng.randrange(0, 32)])
    out = bytes(out[:head])
    return out + bytes(rng.randrange(256) for _ in range(n - head))


def build_nca(keys, *, ver=3, first=0xC00, rights=None, seed=7, size=150_000, content=0):
    """Program NCA with section 0 = CTR PFS0 (exefs) and section 1 = unencrypted PFS0.

    `first` is where section 0 starts (0xC00 right after the header, or 0x4000 with a gap).
    Returns (nca bytes, plaintext of section 0 PFS0 payload, key area CTR key)."""
    rng = random.Random(seed)
    kak, titlekek = derived(keys)
    ctr_key = bytes(rng.randrange(256) for _ in range(16))  # key-area key, or the title key if rights
    h = bytearray(0xC00)
    h[0x200:0x204] = {3: b"NCA3", 2: b"NCA2"}[ver]
    h[0x205] = content
    h[0x206] = 0
    h[0x220] = 1  # key generation 1 -> master_key_00
    h[0x210:0x218] = struct.pack("<Q", 0x0100000000001000)
    if rights:
        h[0x230:0x240] = rights
    p0 = pfs0([("main", body_bytes(rng, size)), ("main.npdm", b"npdm-" + bytes(range(50)))])
    p1 = pfs0([("logo.bin", body_bytes(rng, 9000, 0.5))])
    body = b""
    start = first
    for i, (p, enc) in enumerate([(p0, 3), (p1, 1)]):
        table = hashlib.sha256(p).digest()
        hd = bytearray(0xF8)
        hd[:0x20] = hashlib.sha256(table).digest()
        hd[0x20:0x24] = struct.pack("<I", len(p))
        hd[0x24] = 2
        hd[0x30:0x38] = struct.pack("<Q", 32)
        hd[0x38:0x40] = struct.pack("<Q", 0x200)
        hd[0x40:0x48] = struct.pack("<Q", len(p))
        data = pad(pad(table, 0x200) + p, 0x200)
        sec_start = start + len(body)
        e = 0x240 + i * 0x10
        h[e : e + 4] = struct.pack("<I", sec_start // 0x200)
        h[e + 4 : e + 8] = struct.pack("<I", (sec_start + len(data)) // 0x200)
        f = 0x400 + i * 0x200
        h[f + 2], h[f + 3], h[f + 4] = 1, 2, enc
        h[f + 8 : f + 8 + 0xF8] = hd
        ctr_hdr = bytes([1, 2, 3, 4, 5, 6, 7, 8 + i])
        h[f + 0x140 : f + 0x148] = ctr_hdr
        if enc == 3:
            data = ctr_crypt(ctr_key, ctr_hdr, data, sec_start)
        body += data
    gap = bytes(first - 0xC00)
    h[0x208:0x210] = struct.pack("<Q", 0xC00 + len(gap) + len(body))
    area = bytearray(rng.randrange(256) for _ in range(64))  # filler when rights-managed
    if not rights:
        area[0x20:0x30] = ctr_key
    h[0x300:0x340] = ecb_enc(kak, bytes(area))
    for i in range(6):
        sec = 0 if (ver != 3 and i >= 2) else i
        h[i * 0x200 : (i + 1) * 0x200] = xts_encrypt(keys["header_key"], bytes(h[i * 0x200 : (i + 1) * 0x200]), sec)
    return bytes(h) + gap + body, p0, ctr_key


# ---- tickets ------------------------------------------------------------------------------------
def ticket_common(rights, enc_title_key):
    t = bytearray(4 + 0x13C + 0x2C0)
    t[:4] = struct.pack("<I", 0x10004)
    b = 4 + 0x13C
    t[b : b + 0x40] = b"Root-CA00000003-XS00000020".ljust(0x40, b"\0")
    t[b + 0x40 : b + 0x50] = enc_title_key
    t[b + 0x160 : b + 0x170] = rights
    return bytes(t)


def ticket_personalized(rights, enc_title_key, n, e, d_unused=None):
    t = bytearray(ticket_common(rights, enc_title_key))
    b = 4 + 0x13C
    key = RSA.construct((n, e))
    ct = PKCS1_OAEP.new(key, hashAlgo=SHA256).encrypt(enc_title_key)
    assert len(ct) == 0x100
    t[b + 0x40 : b + 0x140] = ct
    t[b + 0x141] = 1
    t[b + 0x145] = 0
    return bytes(t)


# ---- driving nsz ---------------------------------------------------------------------------------
def run_nsz(workdir, keys, inputs, *extra):
    """Calls the public tool exactly like its CLI would (nsz -C ...), keys via -k-style prod.keys."""
    import subprocess

    (workdir / "out").mkdir(exist_ok=True)
    (workdir / ".switch").mkdir(exist_ok=True)
    (workdir / ".switch" / "prod.keys").write_text(keys_text(keys))
    for src in inputs:
        cmd = [str(Path(sys.executable).with_name("nsz")), "-C", "-o", str(workdir / "out"), "-t", "1", *extra, str(src)]
        env = dict(os.environ, HOME=str(workdir))
        subprocess.run(cmd, cwd=workdir, env=env, check=True)


def build_xci(secure_files):
    """Minimal gamecard image: header (HEAD at 0x100), cert area, root HFS0 at 0xF000 with a
    `secure` partition. Enough for nsz and for napple's parse_xci; `full` adds the 0x1000 key area."""
    sec = hfs0(secure_files)
    root = hfs0([("secure", sec)])
    root_hdr_size = 0x10 + 0x40 + len(b"secure\0".ljust(0x20, b"\0"))
    h = bytearray(0x200)
    h[0x100:0x104] = b"HEAD"
    h[0x130:0x138] = struct.pack("<Q", 0xF000)
    h[0x138:0x140] = struct.pack("<Q", root_hdr_size)
    h[0x140:0x160] = hashlib.sha256(root[:root_hdr_size]).digest()
    img = bytes(h) + bytes(0xF000 - 0x200)
    img = img[:0x7000] + b"\xAA" * 0x200 + img[0x7200:]
    return img + root


def rewrite_pfs0(nsp_bytes, replace):
    """Rebuilds a PFS0 with some members replaced (name -> bytes); members otherwise untouched."""
    n, st = struct.unpack_from("<II", nsp_bytes, 4)
    base = 0x10 + 0x18 * n + st
    files = []
    for i in range(n):
        off, size, so, _ = struct.unpack_from("<QQII", nsp_bytes, 0x10 + 0x18 * i)
        name = nsp_bytes[0x10 + 0x18 * n + so :].split(b"\0")[0].decode()
        files.append((name, replace.get(name, nsp_bytes[base + off : base + off + size])))
    return pfs0(files)


def main(out):
    import shutil

    out = Path(out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    work = out / ".work"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir()
    keys = make_keys()
    (out / "synthetic_keys.txt").write_text(keys_text(keys))
    _, titlekek = derived(keys)

    # Plain NCAs (kept as the byte-exact reference) and the NSPs nsz compresses.
    ncas = {}
    for name, kw in {
        "c00": dict(first=0xC00, seed=11),
        "g4000": dict(first=0x4000, seed=12),
        "nca2": dict(first=0xC00, ver=2, seed=13),
    }.items():
        nca, _, _ = build_nca(keys, **kw)
        ncas[name] = nca
        (out / f"{name}.nca.bin").write_bytes(nca)
        if name != "nca2":  # nsz leaves NCA2 uncompressed (see README), so there is nothing to wrap
            (work / f"{name}.nsp").write_bytes(pfs0([(hashlib.sha256(nca).hexdigest()[:32] + ".nca", nca)]))
    # Personalized-ticket title: rights NCA + a ticket in COMMON form for nsz (it cannot read RSA
    # tickets); the shipped .nsz gets the personalized ticket swapped in afterwards.
    rights = bytes.fromhex("0100000000001000") + bytes(7) + b"\x01"
    nca, _, title_key = build_nca(keys, rights=rights, first=0xC00, seed=14)
    enc_tk = ecb_enc(titlekek, title_key)
    ncas["rights"] = nca
    (work / "rights.nsp").write_bytes(
        pfs0([(hashlib.sha256(nca).hexdigest()[:32] + ".nca", nca), (rights.hex() + ".tik", ticket_common(rights, enc_tk))])
    )
    # XCI with the c00 NCA
    xci = build_xci([(hashlib.sha256(ncas["c00"]).hexdigest()[:32] + ".nca", ncas["c00"])])
    (work / "c00.xci").write_bytes(xci)

    jobs = [
        ("c00", ["-S"], "c00.nsp", "c00_solid.nsz"),
        ("c00", ["-B", "-s", "14"], "c00.nsp", "c00_block.nsz"),
        ("g4000", ["-S"], "g4000.nsp", "g4000_solid.nsz"),
        ("g4000", ["-B", "-s", "15"], "g4000.nsp", "g4000_block.nsz"),
        ("rights", ["-S"], "rights.nsp", "rights_solid.nsz"),
        ("c00", ["-B", "-s", "14"], "c00.xci", "c00.xcz"),
    ]
    for _, flags, src, dst in jobs:
        sub = work / dst.replace(".", "_")
        sub.mkdir()
        run_nsz(sub, keys, [work / src], *flags)
        produced = next((sub / "out").iterdir())
        shutil.copy(produced, out / (dst + ".bin"))
        print(dst, produced.stat().st_size)
    # Personalized ticket: RSA-OAEP(titlekek-encrypted title key) to the throwaway test key.
    n, e = RSA_N, 65537
    tik = ticket_personalized(rights, enc_tk, n, e)
    rn = (out / "rights_solid.nsz.bin").read_bytes()
    (out / "rights_solid.nsz.bin").write_bytes(rewrite_pfs0(rn, {rights.hex() + ".tik": tik}))
    (out / "rights_enc_title_key.hex").write_text(enc_tk.hex() + "\n")
    shutil.rmtree(work)


if __name__ == "__main__":
    main(sys.argv[1])
