#!/usr/bin/env python3
"""Stopgap module-table writer (replaced by the Rust rewriter, see docs/architecture/module-format.md).

Reads the files the rewriter produces for each module in <modules-dir>:
    <name>.patched.bin  <name>.fixups.tsv  <name>.gates.tsv   (+ <name>.sites.tsv, not needed at run time)
plus the original NSOs (<exefs-dir>/<name>) for the build id, and writes
    <name>.tables      binary tables read by the runtime loader (format NTB1, below)
    manifest.json      module order, sizes, sha256 and build ids (no game bytes)

NTB1 (little endian):
    char magic[4] = "NTB1"; u32 version = 1;
    u32 nsym, nrel, ngate, strtab_size;
    u64 file_size, text_end, ro_off, data_off, patch_off, key_off;
    u64 dt_init, dt_fini, init_array, init_array_size, fini_array, fini_array_size;
    u8  build_id[32];
    sym[nsym]  { u32 name_off; u8 binding, type, defined, pad; u64 value, size; }           24 bytes
    rel[nrel]  { u64 where; i64 addend; u32 kind; u32 symbol; }                             24 bytes
    gate[ngate]{ u64 ret; u64 original; u32 timer; u32 arg; }                               24 bytes
    char strtab[strtab_size]
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / 'refpy'))
from relr import decode_relr  # noqa: E402

KINDS = (257, 1025, 1026, 1027)  # R_AARCH64_ABS64, GLOB_DAT, JUMP_SLOT, RELATIVE


def read_layout(path):
    layout = {}
    for line in path.read_text().splitlines():
        if line.startswith('# '):
            key, _, value = line[2:].partition('\t')
            try:
                layout[key] = int(value, 0)
            except ValueError:
                pass
    return layout


def dynamic(image):
    mod0 = struct.unpack_from('<I', image, 4)[0]
    assert image[mod0:mod0 + 4] == b'MOD0', 'MOD0 missing'
    dyn = mod0 + struct.unpack_from('<i', image, mod0 + 4)[0]
    tags = {}
    for off in range(dyn, dyn + 4096 * 16, 16):
        tag, value = struct.unpack_from('<qQ', image, off)
        if tag == 0:
            return tags
        tags.setdefault(tag, value)
    raise ValueError('unterminated dynamic table')


def build(name, mods, exefs, out):
    image = (mods / f'{name}.patched.bin').read_bytes()
    layout = read_layout(mods / f'{name}.fixups.tsv')
    nso = (exefs / name).read_bytes() if (exefs / name).exists() else None
    tags = dynamic(image)
    if 4 in tags:
        nsym = struct.unpack_from('<I', image, tags[4] + 4)[0]
    else:  # GNU hash only: the NSO header gives the exact dynsym extent
        assert nso, 'GNU-hash module needs the original NSO'
        _, _, sym_off, sym_size = struct.unpack_from('<IIII', nso, 0x90)
        assert layout['RO_OFF'] + sym_off == tags[6] and sym_size % 24 == 0
        nsym = sym_size // 24
    assert tags[11] == 24 and tags[9] == 24 and tags.get(20, 7) == 7
    strtab, offsets = bytearray(b'\0'), {}
    syms = []
    for i in range(nsym):
        no, info, _other, section, value, size = struct.unpack_from('<IBBHQQ', image, tags[6] + 24 * i)
        end = image.index(0, tags[5] + no, tags[5] + tags[10])
        s = image[tags[5] + no:end]
        if s not in offsets:
            offsets[s] = len(strtab)
            strtab += s + b'\0'
        syms.append((offsets[s], info >> 4, info & 15, 1 if section else 0, value, size))
    key_off, data_off = layout['KEY_PAGE_OFF'], layout['DATA_OFF']
    rels = [(w, a, 1027, 0) for w, a in decode_relr(image, tags, data_off, key_off)]
    seen = {r[0] for r in rels}
    for addr, size in ((tags.get(7), tags.get(8, 0)), (tags.get(23), tags.get(2, 0))):
        if not addr:
            continue
        assert size % 24 == 0
        for off in range(addr, addr + size, 24):
            where, info, addend = struct.unpack_from('<QQq', image, off)
            kind, si = info & 0xffffffff, info >> 32
            assert kind in KINDS and si < nsym, f'{name}: relocation kind {kind} symbol {si}'
            assert data_off <= where <= key_off - 8, f'{name}: relocation outside data'
            assert where not in seen, f'{name}: duplicate relocation at {where:#x}'
            seen.add(where)
            rels.append((where, addend, kind, si))
    gates = []
    for line in (mods / f'{name}.gates.tsv').read_text().splitlines():
        if not line.strip():
            continue
        ret, kind, arg, original = line.split()
        assert kind in ('cntpct', 'cntvct', 'cntfrq', 'svc'), kind
        gates.append((int(ret, 0), int(original, 0), {'svc': 0, 'cntfrq': 2}.get(kind, 1), int(arg)))
    build_id = nso[0x40:0x60] if nso else bytes(32)
    hdr = struct.pack('<4sIIIII', b'NTB1', 1, len(syms), len(rels), len(gates), len(strtab))
    hdr += struct.pack('<6Q', len(image), layout['TEXT_END'], layout['RO_OFF'], data_off, layout['PATCH_OFF'], key_off)
    hdr += struct.pack('<6Q', tags.get(12, 0), tags.get(13, 0), tags.get(25, 0), tags.get(27, 0), tags.get(26, 0), tags.get(28, 0))
    hdr += build_id
    body = b''.join(struct.pack('<IBBBBQQ', n, b, t, d, 0, v, s) for n, b, t, d, v, s in syms)
    body += b''.join(struct.pack('<QqII', w, a, k, si) for w, a, k, si in rels)
    body += b''.join(struct.pack('<QQII', r, o, t, a) for r, o, t, a in gates)
    (out / f'{name}.tables').write_bytes(hdr + body + bytes(strtab))
    return dict(name=name, file_size=len(image), sha256=hashlib.sha256(image).hexdigest(),
                build_id=build_id.hex(), symbols=len(syms), relocs=len(rels), gates=len(gates))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('modules', type=Path, help='directory with the rewriter outputs')
    p.add_argument('--exefs', type=Path, required=True, help='directory with the original NSOs')
    p.add_argument('--out', type=Path, required=True, help='data directory modules/ output')
    p.add_argument('--title-id', required=True)
    p.add_argument('--npdm', type=Path, help='main.npdm to copy next to manifest.json')
    p.add_argument('--link', action='store_true', help='symlink the patched images instead of copying (saves disk)')
    a = p.parse_args()
    names = sorted({f.name.split('.')[0] for f in a.modules.glob('*.patched.bin')})
    sdk = [n for n in names if n == 'sdk']
    sub = sorted((n for n in names if n.startswith('subsdk')), key=lambda n: int(n[6:]))
    order = [n for n in names if n == 'main'] + sub + sdk
    a.out.mkdir(parents=True, exist_ok=True)
    mods = []
    for n in order:
        mods.append(build(n, a.modules, a.exefs, a.out))
        dst = a.out / f'{n}.patched.bin'
        if dst.exists() or dst.is_symlink():
            dst.unlink()
        src = (a.modules / f'{n}.patched.bin').resolve()
        dst.symlink_to(src) if a.link else dst.write_bytes(src.read_bytes())
    manifest = dict(format=1, title_id=a.title_id, modules=mods, load_order=order,
                    build_id=mods[0]['build_id'][:16] if mods else 'unknown')
    (a.out.parent / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    if a.npdm:
        (a.out.parent / 'main.npdm').write_bytes(a.npdm.read_bytes())
    print(json.dumps({m['name']: (m['symbols'], m['relocs'], m['gates']) for m in mods}))


if __name__ == '__main__':
    main()
