#!/usr/bin/env python3
"""Whole-program test of the AArch32 lifter: compile the C programs in tests/a32jit/progs/ for armv7a (ARM and Thumb-2, NEON + VFPv4, with the system clang),
run each lifted and in the interpreter (progtest.c) and compare with the same source compiled for the host.

  python3 tests/a32jit/progtest.py [name ...]

A program defines `unsigned entry(unsigned a, unsigned b, unsigned c, unsigned d)` first in the file and keeps all data on the stack (no relocations: the
flat .text is the whole image). `// args: a b c d` lines list the argument sets to run; the host result must equal the guest result."""
import ctypes, os, re, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', '..', 'src')
TMP = '/tmp/a32prog_%d' % os.getpid()


def text_section(path):
    d = open(path, 'rb').read()
    shoff, shentsize, shnum, shstrndx = struct.unpack_from('<I', d, 0x20)[0], *struct.unpack_from('<HHH', d, 0x2e)
    secs = [struct.unpack_from('<IIIIIIIIII', d, shoff + i * shentsize) for i in range(shnum)]
    strtab = secs[shstrndx]
    names = {}
    out = None
    for s in secs:
        n = d[strtab[4] + s[0]:].split(b'\0')[0].decode()
        names[n] = s
    if '.rel.text' in names and names['.rel.text'][5] > 0:
        raise SystemExit('%s: .text has relocations; keep the program free of globals and external calls' % path)
    t = names['.text']
    return d[t[4]:t[4] + t[5]]


def main():
    os.makedirs(TMP, exist_ok=True)
    runner = os.path.join(TMP, 'progtest')
    a32 = [os.path.join(SRC, 'a32', f) for f in sorted(os.listdir(os.path.join(SRC, 'a32'))) if f.endswith('.c') and f not in ('a32_module.c', 'a32_proc.c', 'a32_hle.c')]
    subprocess.check_call(['cc', '-std=gnu11', '-O1', '-g', '-DNAPPLE_A32_INTERP', '-I' + os.path.join(SRC, 'a32'), '-o', runner, os.path.join(HERE, 'progtest.c'),
                           os.path.join(SRC, 'a32jit', 'a32lift.c')] + a32)
    progs = sorted(f for f in os.listdir(os.path.join(HERE, 'progs')) if f.endswith('.c'))
    if sys.argv[1:]:
        progs = [p for p in progs if p[:-2] in sys.argv[1:]]
    fails = 0
    for p in progs:
        src = os.path.join(HERE, 'progs', p)
        text = open(src).read()
        argsets = [[int(x, 0) for x in m.split()] for m in re.findall(r'^// args:(.*)$', text, re.M)] or [[1, 2, 3, 4]]
        host = os.path.join(TMP, p[:-2] + '_host.dylib')
        subprocess.check_call(['clang', '-O2', '-shared', '-fno-builtin', '-o', host, src])
        hf = ctypes.CDLL(host).entry
        hf.restype = ctypes.c_uint32
        hf.argtypes = [ctypes.c_uint32] * 4
        for mode in ('arm', 'thumb'):
            obj = os.path.join(TMP, '%s_%s.o' % (p[:-2], mode))
            subprocess.check_call(['clang', '-target', 'armv7a-none-eabihf', '-mcpu=cortex-a15', '-mfpu=neon-vfpv4', '-O2', '-m' + mode, '-ffreestanding', '-fno-builtin',
                                   '-fno-jump-tables', '-c', src, '-o', obj])
            binf = obj[:-2] + '.bin'
            open(binf, 'wb').write(text_section(obj))
            for a in argsets:
                a = (a + [0, 0, 0, 0])[:4]
                r = subprocess.run([runner, binf, '1' if mode == 'thumb' else '0'] + [str(x) for x in a], capture_output=True, text=True)
                want = hf(*a)
                got = int(re.search(r'r0=([0-9a-f]+)', r.stdout).group(1), 16) if 'r0=' in r.stdout else None
                ok = r.returncode == 0 and got == want
                info = r.stderr.strip().splitlines()[-1] if r.stderr.strip() else ''
                print('%-14s %-5s args %-28s %s host %08x guest %s  [%s]' % (p[:-2], mode, a, 'ok  ' if ok else 'FAIL', want, '%08x' % got if got is not None else '-', info))
                if not ok:
                    fails += 1
                    print(r.stderr)
    subprocess.call(['rm', '-rf', TMP])
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
