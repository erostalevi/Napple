#!/usr/bin/env python3
"""addr2sym.py <data-dir> module+0xoff ... : nearest exported symbol (demangled with c++filt when available) for module offsets."""
import struct, subprocess, sys, bisect
d = sys.argv[1]
def load(m):
    b = open(f'{d}/modules/{m}.tables', 'rb').read()
    _, _, ns, nr, ng, ss = struct.unpack_from('<4sIIIII', b, 0)
    off = 152; st = off + (ns + nr + ng) * 24; strtab = b[st:st + ss]
    syms = []
    for i in range(ns):
        n, bd, t, df, _, v, s = struct.unpack_from('<IBBBBQQ', b, off + 24 * i)
        if df and t == 2: syms.append((v, s, strtab[n:strtab.index(0, n)].decode()))
    syms.sort(); return syms
cache = {}
for a in sys.argv[2:]:
    m, o = a.split('+'); o = int(o, 16)
    syms = cache.setdefault(m, load(m)); i = bisect.bisect_right([s[0] for s in syms], o) - 1
    name = syms[i][2] if i >= 0 else '?'
    try: name = subprocess.run(['c++filt', name], capture_output=True, text=True).stdout.strip()
    except Exception: pass
    print(a, '->', name, hex(o - syms[i][0]) if i >= 0 else '')
